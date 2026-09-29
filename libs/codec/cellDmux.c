/*
 * ps3recomp - cellDmux HLE implementation
 *
 * PAMF is an MPEG-2 Program Stream.  Earlier revisions treated every buffer
 * passed to cellDmuxSetStream as one AU for every enabled ES.  That made a
 * complete movie (typically fed in a handful of 512 KiB chunks) look like
 * only a handful of video pictures, and it also handed muxed PAMF bytes to
 * cellVdec/cellAdec instead of elementary-stream access units.
 *
 * This implementation parses pack/PES headers, routes PES payload by the
 * CellDmuxEsFilterId, and reconstructs AVC/MPEG-2 video and ATRAC3+ access
 * units across PES and SetStream boundaries.  The parsing rules intentionally
 * mirror the format contract used by Sony PAMF streams and RPCS3's PAMF
 * demuxer, while keeping the HLE state compact and host-side.
 */

#include "cellDmux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <limits.h>
#include "../../runtime/ppu/ppu_memory.h"
#include "../guest_struct.h"

#ifdef _WIN32
#include <windows.h>
#define DMUX_YIELD() SwitchToThread()
#else
#include <sched.h>
#define DMUX_YIELD() sched_yield()
#endif

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;

/* -------------------------------------------------------------------------
 * PAMF / MPEG-PS constants
 * ---------------------------------------------------------------------- */

#define PAMF_PACK_START          0xBAu
#define PAMF_SYSTEM_HEADER       0xBBu
#define PAMF_PROGRAM_END         0xB9u
#define PAMF_PRIVATE_STREAM_1    0xBDu
#define PAMF_PADDING_STREAM      0xBEu
#define PAMF_PRIVATE_STREAM_2    0xBFu

/* The real PAMF module exposes a 64-entry queue.  This HLE parses SetStream
 * synchronously instead of on Sony's SPU worker, so it needs a deeper host
 * queue to avoid blocking the SetStream caller before sibling guest threads
 * get scheduled to drain callbacks.  Guest-visible sizing remains Sony-like. */
#define DMUX_AU_QUEUE_CAP        1024u
#define DMUX_GUEST_META_SIZE     0x100u
#define DMUX_VIDEO_DEFAULT_MAX   0xCC000u
#define DMUX_ATRAC_MAX           0x1008u
#define DMUX_AUDIO_TICKS_48K     3840u /* 2048 samples * 90000 / 48000 */
#define DMUX_INVALID_TS          UINT64_MAX

/* Sony / RPCS3 maximum AVC AU sizes by H.264 level. */
static u32 pamf_avc_au_max(u32 level)
{
    switch (level) {
        case 21: return 0x12900u;
        case 30: return 0x25F80u;
        case 31: return 0x54600u;
        case 32: return 0x78000u;
        case 41: return 0xC0000u;
        case 42: return 0xCC000u;
        default: return DMUX_VIDEO_DEFAULT_MAX;
    }
}

/* Sony's PAMF ES work-buffer sizing is much larger than one AU because it
 * contains a 64-entry queue.  Returning a realistic size matters: titles
 * allocate this memory from QueryEsAttr and expect it to be large enough for
 * the level advertised by the PAMF header. */
static u32 pamf_avc_es_mem_size(u32 level)
{
    u32 queue_buf;
    switch (level) {
        case 21: queue_buf = 0x0B00C0u; break;
        case 30: queue_buf = 0x19F2E0u; break;
        case 31: queue_buf = 0x260120u; break;
        case 32: queue_buf = 0x35F6C0u; break;
        case 41: queue_buf = 0x45E870u; break;
        case 42: queue_buf = 0x46A870u; break;
        default: queue_buf = 0x46A870u; break;
    }
    /* Opaque Sony bookkeeping is small relative to the queue.  Keep enough
     * headroom for the guest-visible AU/message area used by this HLE. */
    return queue_buf + 0x2000u;
}

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

typedef struct {
    int in_use;
    u32 cbFunc;
    u32 cbArg;
    u32 resMemAddr;
    u32 resMemSize;
    u32 streamType;
    u32 streamAddr;
    u32 streamSize;
    u64 userData;
    u32 auSeqNo;
} DmuxSlot;

typedef struct {
    u8* data;
    u32 size;
    u64 userData;
    u64 pts;
    u64 dts;
    u32 isRap;
} DmuxAuEntry;

typedef struct {
    int in_use;
    u32 dmuxId;
    CellDmuxEsFilterId filterId;
    u32 esCbFunc;
    u32 esCbArg;
    u32 memAddr;
    u32 memSize;
    u32 auMaxSize;

    DmuxAuEntry queue[DMUX_AU_QUEUE_CAP];
    atomic_uint qHeadSeq; /* monotonically increasing; consumer owns writes */
    atomic_uint qTailSeq; /* monotonically increasing; producer owns writes */
    atomic_int acquired;

    /* Partial ES access unit assembled across PES / SetStream calls. */
    u8* build;
    size_t buildSize;
    size_t buildCap;
    int buildStarted;
    u64 buildPts;
    u64 buildDts;
    u64 buildUserData;
    int buildRap;

    /* Timestamp carried by the PES that starts the next delimiter. */
    int pendingTs;
    u64 pendingPts;
    u64 pendingDts;
    u64 pendingUserData;

    /* ATRAC3+ rolling state. */
    u64 audioNextPts;
    int audioPtsValid;

    /* PRIVATE_STREAM_2 can mark the next video AU as random access. */
    int nextRap;
} DmuxEsSlot;

static DmuxSlot s_dmux[CELL_DMUX_MAX_HANDLES];
static DmuxEsSlot s_es[CELL_DMUX_MAX_ES];

static unsigned es_q_count(const DmuxEsSlot* es)
{
    const unsigned head = atomic_load_explicit(&es->qHeadSeq, memory_order_acquire);
    const unsigned tail = atomic_load_explicit(&es->qTailSeq, memory_order_acquire);
    return tail - head;
}

static void free_entry(DmuxAuEntry* e)
{
    free(e->data);
    memset(e, 0, sizeof(*e));
}

static void clear_es_queue(DmuxEsSlot* es)
{
    const unsigned head = atomic_load_explicit(&es->qHeadSeq, memory_order_relaxed);
    const unsigned tail = atomic_load_explicit(&es->qTailSeq, memory_order_relaxed);
    for (unsigned seq = head; seq != tail; ++seq)
        free_entry(&es->queue[seq % DMUX_AU_QUEUE_CAP]);
    atomic_store_explicit(&es->qHeadSeq, 0, memory_order_release);
    atomic_store_explicit(&es->qTailSeq, 0, memory_order_release);
    atomic_store_explicit(&es->acquired, 0, memory_order_release);
}

static void reset_builder(DmuxEsSlot* es)
{
    es->buildSize = 0;
    es->buildStarted = 0;
    es->buildPts = DMUX_INVALID_TS;
    es->buildDts = DMUX_INVALID_TS;
    es->buildUserData = 0;
    es->buildRap = 0;
    es->pendingTs = 0;
    es->pendingPts = DMUX_INVALID_TS;
    es->pendingDts = DMUX_INVALID_TS;
    es->pendingUserData = 0;
    es->audioPtsValid = 0;
    es->audioNextPts = DMUX_INVALID_TS;
    es->nextRap = 0;
}

static void destroy_es(DmuxEsSlot* es)
{
    clear_es_queue(es);
    free(es->build);
    memset(es, 0, sizeof(*es));
}

static int ensure_build_capacity(DmuxEsSlot* es, size_t need)
{
    if (need <= es->buildCap) return 1;
    size_t cap = es->buildCap ? es->buildCap : 4096u;
    while (cap < need) {
        if (cap > (size_t)UINT_MAX / 2u) return 0;
        cap *= 2u;
    }
    u8* p = (u8*)realloc(es->build, cap);
    if (!p) return 0;
    es->build = p;
    es->buildCap = cap;
    return 1;
}

static void notify_es(DmuxEsSlot* es, u32 esHandle, u32 msgType, u64 userData)
{
    if (!es->esCbFunc || !g_ps3_guest_caller) return;
    u32 msgEa = es->memAddr ? es->memAddr + 0x40u : 0;
    if (msgEa) {
        vm_write32(msgEa + 0x00, msgType);
        vm_write32(msgEa + 0x04, 0);
        vm_write64(msgEa + 0x08, userData);
    }
    g_ps3_guest_caller(es->esCbFunc, (u64)es->dmuxId, (u64)esHandle,
                       (u64)msgEa, (u64)es->esCbArg, 0, 0, 0, 0);
}

static s32 queue_au(u32 esHandle, const u8* data, size_t size,
                    u64 pts, u64 dts, u64 userData, int isRap)
{
    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use || !data || !size)
        return (s32)CELL_DMUX_ERROR_ARG;
    if (size > UINT_MAX) return (s32)CELL_DMUX_ERROR_FATAL;

    DmuxEsSlot* es = &s_es[esHandle];
    if (size > es->auMaxSize) {
        printf("[cellDmux] ES %u: AU size %zu exceeds max 0x%X\n", esHandle, size, es->auMaxSize);
        return (s32)CELL_DMUX_ERROR_FATAL;
    }

    while (es_q_count(es) >= DMUX_AU_QUEUE_CAP) {
        /* cellDmux is allowed to back-pressure when the AU queue is full. */
        DMUX_YIELD();
    }

    unsigned tail = atomic_load_explicit(&es->qTailSeq, memory_order_relaxed);
    DmuxAuEntry* e = &es->queue[tail % DMUX_AU_QUEUE_CAP];
    free_entry(e);
    e->data = (u8*)malloc(size);
    if (!e->data) return (s32)CELL_DMUX_ERROR_FATAL;
    memcpy(e->data, data, size);
    e->size = (u32)size;
    e->pts = pts;
    e->dts = dts;
    e->userData = userData;
    e->isRap = isRap ? 1u : 0u;

    atomic_store_explicit(&es->qTailSeq, tail + 1u, memory_order_release);

    if (tail < 4u || (tail & 31u) == 31u) {
        printf("[cellDmux] ES %u: AU_FOUND #%u size=%u pts=%llu rap=%u queued=%u\n",
               esHandle, tail + 1u, e->size, (unsigned long long)e->pts,
               e->isRap, es_q_count(es));
    }
    notify_es(es, esHandle, CELL_DMUX_ES_MSG_TYPE_AU_FOUND, userData);
    return CELL_OK;
}

static int is_h264_idr(const u8* p, size_t n)
{
    for (size_t i = 0; i + 4u <= n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if ((p[i + 3] & 0x1Fu) == 5u) return 1;
        }
    }
    return 0;
}

static s32 finalize_video_au(u32 esHandle)
{
    DmuxEsSlot* es = &s_es[esHandle];
    if (!es->buildStarted || !es->buildSize) return CELL_OK;
    const int rap = es->buildRap || is_h264_idr(es->build, es->buildSize);
    s32 rc = queue_au(esHandle, es->build, es->buildSize,
                      es->buildPts, es->buildDts, es->buildUserData, rap);
    es->buildSize = 0;
    es->buildStarted = 0;
    es->buildPts = DMUX_INVALID_TS;
    es->buildDts = DMUX_INVALID_TS;
    es->buildUserData = 0;
    es->buildRap = 0;
    return rc;
}

static void begin_video_au(DmuxEsSlot* es, u64 fallbackUserData)
{
    es->buildStarted = 1;
    if (es->pendingTs) {
        es->buildPts = es->pendingPts;
        es->buildDts = es->pendingDts;
        es->buildUserData = es->pendingUserData;
        es->pendingTs = 0;
    } else {
        es->buildPts = DMUX_INVALID_TS;
        es->buildDts = DMUX_INVALID_TS;
        es->buildUserData = fallbackUserData;
    }
    es->buildRap = es->nextRap;
    es->nextRap = 0;
}

/* Feed video ES bytes and split at AVC AUD (00 00 01 09) or MPEG-2 picture /
 * sequence start codes.  The delimiter belongs to the AU that starts there. */
static s32 feed_video(u32 esHandle, const u8* data, size_t size,
                      u64 pts, u64 dts, int hasTs, u64 userData)
{
    DmuxEsSlot* es = &s_es[esHandle];
    const int avc = es->filterId.supplementalInfo1 != 0;

    if (hasTs) {
        es->pendingTs = 1;
        es->pendingPts = pts;
        es->pendingDts = dts;
        es->pendingUserData = userData;
    }

    for (size_t i = 0; i < size; ++i) {
        if (!ensure_build_capacity(es, es->buildSize + 1u))
            return (s32)CELL_DMUX_ERROR_FATAL;
        es->build[es->buildSize++] = data[i];

        if (es->buildSize < 4u) continue;
        size_t d = es->buildSize - 4u;
        if (!(es->build[d] == 0 && es->build[d + 1] == 0 && es->build[d + 2] == 1))
            continue;

        const u8 code = es->build[d + 3];
        const int delimiter = avc ? (code == 0x09u)
                                  : (code == 0x00u || code == 0xB3u || code == 0xB7u);
        if (!delimiter) continue;

        if (!es->buildStarted) {
            /* Bytes before the first delimiter are pack/PES residue or legal
             * Annex-B leading zeros.  Keep only the delimiter onward. */
            if (d) {
                memmove(es->build, es->build + d, 4u);
                es->buildSize = 4u;
            }
            begin_video_au(es, userData);
            continue;
        }

        if (d == 0) continue;

        /* The new delimiter has already been appended.  Temporarily remove it,
         * publish the previous AU, then seed the next AU with the delimiter. */
        u8 delim[4];
        memcpy(delim, es->build + d, 4u);
        es->buildSize = d;
        s32 rc = finalize_video_au(esHandle);
        if (rc != CELL_OK) return rc;
        if (!ensure_build_capacity(es, 4u)) return (s32)CELL_DMUX_ERROR_FATAL;
        memcpy(es->build, delim, 4u);
        es->buildSize = 4u;
        begin_video_au(es, userData);
    }

    return CELL_OK;
}

static s32 feed_atrac(u32 esHandle, const u8* data, size_t size,
                      u64 pts, u64 dts, int hasTs, u64 userData)
{
    DmuxEsSlot* es = &s_es[esHandle];
    if (hasTs) {
        es->audioNextPts = pts;
        es->audioPtsValid = 1;
        es->pendingDts = dts;
    }

    if (!ensure_build_capacity(es, es->buildSize + size))
        return (s32)CELL_DMUX_ERROR_FATAL;
    memcpy(es->build + es->buildSize, data, size);
    es->buildSize += size;

    for (;;) {
        if (es->buildSize < 4u) break;

        /* Recover from malformed/unknown private-stream payload by searching
         * for the ATRAC-X ATS sync word. */
        if (!(es->build[0] == 0x0Fu && es->build[1] == 0xD0u)) {
            size_t off = 1;
            while (off + 1u < es->buildSize &&
                   !(es->build[off] == 0x0Fu && es->build[off + 1u] == 0xD0u))
                ++off;
            memmove(es->build, es->build + off, es->buildSize - off);
            es->buildSize -= off;
            if (es->buildSize < 4u) break;
        }

        const u16 info = (u16)(((u16)es->build[2] << 8) | es->build[3]);
        const u16 n = (u16)(info & 0x03FFu);
        if (n >= 0x0200u) {
            memmove(es->build, es->build + 1u, --es->buildSize);
            continue;
        }
        const size_t frameSize = ((size_t)n + 1u) * 8u + 8u;
        if (frameSize > DMUX_ATRAC_MAX || es->buildSize < frameSize) break;

        u64 outPts = es->audioPtsValid ? es->audioNextPts : DMUX_INVALID_TS;
        u64 outDts = es->audioPtsValid ? es->pendingDts : DMUX_INVALID_TS;
        s32 rc = queue_au(esHandle, es->build, frameSize, outPts, outDts, userData, 0);
        if (rc != CELL_OK) return rc;
        if (es->audioPtsValid) {
            es->audioNextPts += DMUX_AUDIO_TICKS_48K;
            es->pendingDts = es->audioNextPts;
        }
        memmove(es->build, es->build + frameSize, es->buildSize - frameSize);
        es->buildSize -= frameSize;
    }
    return CELL_OK;
}

static u64 parse_pes_ts(const u8* p)
{
    return (((u64)(p[0] >> 1) & 0x07u) << 30) |
           ((u64)p[1] << 22) |
           (((u64)p[2] >> 1) << 15) |
           ((u64)p[3] << 7) |
           ((u64)p[4] >> 1);
}

static void mark_video_rap(u32 dmuxId, u8 channel)
{
    const u8 sid = (u8)(0xE0u | (channel & 0x0Fu));
    for (u32 i = 0; i < CELL_DMUX_MAX_ES; ++i) {
        if (s_es[i].in_use && s_es[i].dmuxId == dmuxId &&
            (u8)s_es[i].filterId.filterIdMajor == sid)
            s_es[i].nextRap = 1;
    }
}

static s32 dispatch_pes(u32 dmuxId, u8 sid, const u8* pes, size_t total, u64 userData)
{
    if (total < 9u) return CELL_OK;
    const u8 flags = pes[7];
    const size_t hdrLen = pes[8];
    const size_t payloadOff = 9u + hdrLen;
    if (payloadOff > total) return (s32)CELL_DMUX_ERROR_FATAL;

    u64 pts = DMUX_INVALID_TS, dts = DMUX_INVALID_TS;
    int hasTs = 0;
    if ((flags & 0x80u) && hdrLen >= 5u && total >= 14u) {
        pts = parse_pes_ts(pes + 9u);
        dts = pts;
        hasTs = 1;
        if ((flags & 0x40u) && hdrLen >= 10u && total >= 19u)
            dts = parse_pes_ts(pes + 14u);
    }

    const u8* payload = pes + payloadOff;
    size_t payloadSize = total - payloadOff;

    if (sid >= 0xE0u && sid <= 0xEFu) {
        for (u32 i = 0; i < CELL_DMUX_MAX_ES; ++i) {
            if (!s_es[i].in_use || s_es[i].dmuxId != dmuxId) continue;
            if ((u8)s_es[i].filterId.filterIdMajor != sid) continue;
            s32 rc = feed_video(i, payload, payloadSize, pts, dts, hasTs, userData);
            if (rc != CELL_OK) return rc;
        }
        return CELL_OK;
    }

    if (sid == PAMF_PRIVATE_STREAM_1 && payloadSize >= 4u) {
        const u8 subId = payload[0];
        /* PAMF private_stream_1 has a 4-byte substream header before ES data. */
        payload += 4u;
        payloadSize -= 4u;
        for (u32 i = 0; i < CELL_DMUX_MAX_ES; ++i) {
            if (!s_es[i].in_use || s_es[i].dmuxId != dmuxId) continue;
            if ((u8)s_es[i].filterId.filterIdMajor != PAMF_PRIVATE_STREAM_1) continue;
            if ((u8)s_es[i].filterId.filterIdMinor != subId) continue;

            /* 0x00..0x0f are ATRAC3+ channels in PAMF.  Other private stream
             * codecs are still delivered per-PES rather than muxed chunks. */
            if ((subId & 0xF0u) == 0x00u) {
                s32 rc = feed_atrac(i, payload, payloadSize, pts, dts, hasTs, userData);
                if (rc != CELL_OK) return rc;
            } else if (payloadSize) {
                s32 rc = queue_au(i, payload, payloadSize, pts, dts, userData, 0);
                if (rc != CELL_OK) return rc;
            }
        }
    }
    return CELL_OK;
}

static s32 parse_program_stream(u32 dmuxId, const u8* src, size_t size, u64 userData)
{
    size_t p = 0;
    unsigned pesCount = 0;
    while (p + 4u <= size) {
        if (!(src[p] == 0 && src[p + 1] == 0 && src[p + 2] == 1)) {
            ++p;
            continue;
        }

        const u8 sid = src[p + 3];
        if (sid == PAMF_PACK_START) {
            if (p + 14u > size) break;
            p += 14u + (src[p + 13u] & 0x07u);
            continue;
        }
        if (sid == PAMF_PROGRAM_END) {
            p += 4u;
            continue;
        }
        if (p + 6u > size) break;

        const size_t payloadLen = ((size_t)src[p + 4u] << 8) | src[p + 5u];
        const size_t total = 6u + payloadLen;
        if (total < 6u || p + total > size) {
            printf("[cellDmux] truncated PS packet sid=0x%02X at %zu (%zu/%zu)\n",
                   sid, p, total, size - p);
            break;
        }

        if (sid == PAMF_PRIVATE_STREAM_2) {
            if (payloadLen >= 2u) {
                const u16 streamId = (u16)(((u16)src[p + 6u] << 8) | src[p + 7u]);
                mark_video_rap(dmuxId, (u8)(streamId & 0x0Fu));
            }
        } else if (sid == PAMF_PRIVATE_STREAM_1 || (sid >= 0xE0u && sid <= 0xEFu)) {
            s32 rc = dispatch_pes(dmuxId, sid, src + p, total, userData);
            if (rc != CELL_OK) return rc;
            ++pesCount;
        } else {
            /* system header, program map, padding, and other PS packets */
            (void)PAMF_SYSTEM_HEADER;
            (void)PAMF_PADDING_STREAM;
        }
        p += total;
    }

    printf("[cellDmux] parsed PAMF chunk: %zu bytes, %u PES packets\n", size, pesCount);
    return CELL_OK;
}

/* -------------------------------------------------------------------------
 * Query attributes
 * ---------------------------------------------------------------------- */

s32 cellDmuxQueryAttr(const CellDmuxType* type, CellDmuxAttr* attr)
{
    printf("[cellDmux] QueryAttr(type=%u)\n", type ? vm_read32(GUEST_EA(type)) : 0);
    if (!type || !attr) return (s32)CELL_DMUX_ERROR_ARG;
    u32 ea = GUEST_EA(attr);
    vm_write32(ea + (u32)offsetof(CellDmuxAttr, memSize), 64u * 1024u);
    vm_write32(ea + (u32)offsetof(CellDmuxAttr, demuxerVerUpper), 1);
    vm_write32(ea + (u32)offsetof(CellDmuxAttr, demuxerVerLower), 0);
    return CELL_OK;
}

s32 cellDmuxQueryEsAttr(const CellDmuxType* type, const CellDmuxEsFilterId* esFilterId,
                        const void* esSpecificInfo, CellDmuxEsAttr* esAttr)
{
    if (!type || !esFilterId || !esAttr) return (s32)CELL_DMUX_ERROR_ARG;

    CellDmuxEsFilterId f;
    guest_struct_load(&f, GUEST_EA(esFilterId), (u32)sizeof(f));
    u32 memSize = 64u * 1024u;
    if ((u8)f.filterIdMajor >= 0xE0u && (u8)f.filterIdMajor <= 0xEFu && f.supplementalInfo1) {
        const u32 level = esSpecificInfo ? vm_read32(GUEST_EA(esSpecificInfo)) : 42u;
        memSize = pamf_avc_es_mem_size(level);
        printf("[cellDmux] QueryEsAttr AVC level=%u -> memSize=0x%X\n", level, memSize);
    } else {
        printf("[cellDmux] QueryEsAttr filter=%02X/%02X -> memSize=0x%X\n",
               f.filterIdMajor, f.filterIdMinor, memSize);
    }
    vm_write32(GUEST_EA(esAttr) + (u32)offsetof(CellDmuxEsAttr, memSize), memSize);
    return CELL_OK;
}

/* -------------------------------------------------------------------------
 * Demuxer lifecycle
 * ---------------------------------------------------------------------- */

s32 cellDmuxOpen(const CellDmuxType* type, const CellDmuxResource* res,
                 const CellDmuxCb* cb, CellDmuxHandle* handle)
{
    printf("[cellDmux] Open(streamType=%u)\n", type ? vm_read32(GUEST_EA(type)) : 0);
    if (!type || !handle) return (s32)CELL_DMUX_ERROR_ARG;

    u32 cbFunc = 0, cbArg = 0, resMemAddr = 0, resMemSize = 0;
    if (cb) {
        u32 ea = GUEST_EA(cb);
        cbFunc = vm_read32(ea + (u32)offsetof(CellDmuxCb, cbFunc));
        cbArg = vm_read32(ea + (u32)offsetof(CellDmuxCb, cbArg));
    }
    if (res) {
        u32 ea = GUEST_EA(res);
        resMemAddr = vm_read32(ea + (u32)offsetof(CellDmuxResource, memAddr));
        resMemSize = vm_read32(ea + (u32)offsetof(CellDmuxResource, memSize));
    }

    for (u32 i = 0; i < CELL_DMUX_MAX_HANDLES; ++i) {
        if (!s_dmux[i].in_use) {
            memset(&s_dmux[i], 0, sizeof(s_dmux[i]));
            s_dmux[i].in_use = 1;
            s_dmux[i].cbFunc = cbFunc;
            s_dmux[i].cbArg = cbArg;
            s_dmux[i].resMemAddr = resMemAddr;
            s_dmux[i].resMemSize = resMemSize;
            s_dmux[i].streamType = vm_read32(GUEST_EA(type));
            vm_write32(GUEST_EA(handle), i);
            printf("[cellDmux] Open -> handle=%u, cbFunc=0x%08X, cbArg=0x%08X\n", i, cbFunc, cbArg);
            return CELL_OK;
        }
    }
    return (s32)CELL_DMUX_ERROR_BUSY;
}

s32 cellDmuxClose(CellDmuxHandle handle)
{
    printf("[cellDmux] Close(handle=%u)\n", handle);
    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;
    for (u32 i = 0; i < CELL_DMUX_MAX_ES; ++i)
        if (s_es[i].in_use && s_es[i].dmuxId == handle) destroy_es(&s_es[i]);
    memset(&s_dmux[handle], 0, sizeof(s_dmux[handle]));
    return CELL_OK;
}

/* -------------------------------------------------------------------------
 * ES management
 * ---------------------------------------------------------------------- */

s32 cellDmuxEnableEs(CellDmuxHandle handle, const CellDmuxEsFilterId* esFilterId,
                     const CellDmuxEsResource* esRes,
                     const CellDmuxEsCb* esCb, const void* esSpecificInfo,
                     CellDmuxEsHandle* esHandle)
{
    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use || !esFilterId || !esHandle)
        return (s32)CELL_DMUX_ERROR_ARG;

    u32 esCbFunc = 0, esCbArg = 0, memAddr = 0, memSize = 0;
    if (esCb) {
        u32 ea = GUEST_EA(esCb);
        esCbFunc = vm_read32(ea + (u32)offsetof(CellDmuxEsCb, cbFunc));
        esCbArg = vm_read32(ea + (u32)offsetof(CellDmuxEsCb, cbArg));
    }
    if (esRes) {
        u32 ea = GUEST_EA(esRes);
        memAddr = vm_read32(ea + (u32)offsetof(CellDmuxEsResource, memAddr));
        memSize = vm_read32(ea + (u32)offsetof(CellDmuxEsResource, memSize));
    }

    for (u32 i = 0; i < CELL_DMUX_MAX_ES; ++i) {
        if (s_es[i].in_use) continue;
        DmuxEsSlot* es = &s_es[i];
        memset(es, 0, sizeof(*es));
        es->in_use = 1;
        es->dmuxId = handle;
        guest_struct_load(&es->filterId, GUEST_EA(esFilterId), (u32)sizeof(es->filterId));
        es->esCbFunc = esCbFunc;
        es->esCbArg = esCbArg;
        es->memAddr = memAddr;
        es->memSize = memSize;
        atomic_init(&es->qHeadSeq, 0u);
        atomic_init(&es->qTailSeq, 0u);
        atomic_init(&es->acquired, 0);
        reset_builder(es);

        if ((u8)es->filterId.filterIdMajor >= 0xE0u &&
            (u8)es->filterId.filterIdMajor <= 0xEFu) {
            const u32 level = (es->filterId.supplementalInfo1 && esSpecificInfo)
                            ? vm_read32(GUEST_EA(esSpecificInfo)) : 42u;
            es->auMaxSize = es->filterId.supplementalInfo1 ? pamf_avc_au_max(level) : 0x12A800u;
        } else if ((u8)es->filterId.filterIdMajor == PAMF_PRIVATE_STREAM_1 &&
                   ((u8)es->filterId.filterIdMinor & 0xF0u) == 0x00u) {
            es->auMaxSize = DMUX_ATRAC_MAX;
        } else {
            es->auMaxSize = memSize > DMUX_GUEST_META_SIZE ? memSize - DMUX_GUEST_META_SIZE : 0x10000u;
        }

        if (memSize <= DMUX_GUEST_META_SIZE || memSize - DMUX_GUEST_META_SIZE < es->auMaxSize) {
            printf("[cellDmux] EnableEs: resource too small mem=0x%X need >=0x%X\n",
                   memSize, es->auMaxSize + DMUX_GUEST_META_SIZE);
            destroy_es(es);
            return (s32)CELL_DMUX_ERROR_ARG;
        }

        vm_write32(GUEST_EA(esHandle), i);
        printf("[cellDmux] EnableEs -> esHandle=%u filter=%02X/%02X avc=%u mem=0x%08X/0x%X auMax=0x%X\n",
               i, es->filterId.filterIdMajor, es->filterId.filterIdMinor,
               es->filterId.supplementalInfo1, memAddr, memSize, es->auMaxSize);
        return CELL_OK;
    }
    return (s32)CELL_DMUX_ERROR_BUSY;
}

s32 cellDmuxDisableEs(CellDmuxEsHandle esHandle)
{
    printf("[cellDmux] DisableEs(es=%u)\n", esHandle);
    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;
    destroy_es(&s_es[esHandle]);
    return CELL_OK;
}

/* -------------------------------------------------------------------------
 * Data feeding
 * ---------------------------------------------------------------------- */

s32 cellDmuxSetStream(CellDmuxHandle handle, u32 streamAddr, u32 streamSize,
                      b8 discontinuity, u64 userData)
{
    printf("[cellDmux] SetStream(handle=%u, addr=0x%X, size=%u, discont=%d)\n",
           handle, streamAddr, streamSize, discontinuity);
    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use || !streamAddr || !streamSize)
        return (s32)CELL_DMUX_ERROR_ARG;

    DmuxSlot* dmux = &s_dmux[handle];
    dmux->streamAddr = streamAddr;
    dmux->streamSize = streamSize;
    dmux->userData = userData;

    if (discontinuity) {
        dmux->auSeqNo = 0;
        for (u32 i = 0; i < CELL_DMUX_MAX_ES; ++i) {
            if (s_es[i].in_use && s_es[i].dmuxId == handle) {
                clear_es_queue(&s_es[i]);
                reset_builder(&s_es[i]);
            }
        }
    }

    const u8* src = GUEST_PTR((void*)(uintptr_t)streamAddr, const u8*);
    s32 rc = parse_program_stream(handle, src, streamSize, userData);
    if (rc != CELL_OK) return rc;
    dmux->auSeqNo++;

    if (dmux->cbFunc && g_ps3_guest_caller) {
        u32 msgEa = dmux->resMemAddr;
        if (msgEa) {
            vm_write32(msgEa + 0x00, CELL_DMUX_MSG_TYPE_DEMUX_DONE);
            vm_write32(msgEa + 0x04, 0);
            vm_write64(msgEa + 0x08, userData);
        }
        g_ps3_guest_caller(dmux->cbFunc, (u64)handle, (u64)msgEa,
                           (u64)dmux->cbArg, 0, 0, 0, 0, 0);
    }
    return CELL_OK;
}

s32 cellDmuxResetStream(CellDmuxHandle handle)
{
    printf("[cellDmux] ResetStream(handle=%u)\n", handle);
    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;
    for (u32 i = 0; i < CELL_DMUX_MAX_ES; ++i) {
        if (s_es[i].in_use && s_es[i].dmuxId == handle) {
            clear_es_queue(&s_es[i]);
            reset_builder(&s_es[i]);
        }
    }
    s_dmux[handle].streamAddr = 0;
    s_dmux[handle].streamSize = 0;
    s_dmux[handle].auSeqNo = 0;
    return CELL_OK;
}

s32 cellDmuxResetStreamAndWaitDone(CellDmuxHandle handle)
{
    return cellDmuxResetStream(handle);
}

/* -------------------------------------------------------------------------
 * AU retrieval
 * ---------------------------------------------------------------------- */

static s32 write_au_info(CellDmuxEsHandle esHandle, void* auInfo,
                         void* auSpecificInfo, int consume, int is_ex)
{
    (void)is_ex;
    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    DmuxEsSlot* es = &s_es[esHandle];
    unsigned head = atomic_load_explicit(&es->qHeadSeq, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&es->qTailSeq, memory_order_acquire);
    if (head == tail || (consume && atomic_load_explicit(&es->acquired, memory_order_acquire)))
        return (s32)CELL_DMUX_ERROR_EMPTY;

    DmuxAuEntry* e = &es->queue[head % DMUX_AU_QUEUE_CAP];
    if (!e->data || !e->size) return (s32)CELL_DMUX_ERROR_FATAL;
    if (e->size > es->memSize - DMUX_GUEST_META_SIZE)
        return (s32)CELL_DMUX_ERROR_FATAL;

    const u32 guestInfo = es->memAddr;
    const u32 guestData = es->memAddr + DMUX_GUEST_META_SIZE;
    memcpy(vm_translate(guestData), e->data, e->size);

    vm_write32(guestInfo + 0x00, guestData);
    vm_write32(guestInfo + 0x04, e->size);
    vm_write32(guestInfo + 0x08, es->auMaxSize);
    vm_write8 (guestInfo + 0x0C, (u8)(e->isRap ? 1 : 0));
    vm_write8 (guestInfo + 0x0D, 0);
    vm_write8 (guestInfo + 0x0E, 0);
    vm_write8 (guestInfo + 0x0F, 0);
    vm_write64(guestInfo + 0x10, e->userData);
    vm_write64(guestInfo + 0x18, e->pts);
    vm_write64(guestInfo + 0x20, e->dts);

    if (auInfo) vm_write32(GUEST_EA(auInfo), guestInfo);
    if (auSpecificInfo) vm_write32(GUEST_EA(auSpecificInfo), 0);
    if (consume) atomic_store_explicit(&es->acquired, 1, memory_order_release);

    if (head < 4u || (head & 31u) == 31u) {
        printf("[cellDmux] %sAu(es=%u) -> data=0x%08X size=%u pts=%llu queued=%u\n",
               consume ? "Get" : "Peek", esHandle, guestData, e->size,
               (unsigned long long)e->pts, tail - head);
    }
    return CELL_OK;
}

s32 cellDmuxGetAu(CellDmuxEsHandle esHandle, void* auInfo, void* auSpecificInfo)
{ return write_au_info(esHandle, auInfo, auSpecificInfo, 1, 0); }

s32 cellDmuxGetAuEx(CellDmuxEsHandle esHandle, void* auInfoEx, void* auSpecificInfo)
{ return write_au_info(esHandle, auInfoEx, auSpecificInfo, 1, 1); }

s32 cellDmuxPeekAu(CellDmuxEsHandle esHandle, void* auInfo, void* auSpecificInfo)
{ return write_au_info(esHandle, auInfo, auSpecificInfo, 0, 0); }

s32 cellDmuxPeekAuEx(CellDmuxEsHandle esHandle, void* auInfoEx, void* auSpecificInfo)
{ return write_au_info(esHandle, auInfoEx, auSpecificInfo, 0, 1); }

s32 cellDmuxReleaseAu(CellDmuxEsHandle esHandle)
{
    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;
    DmuxEsSlot* es = &s_es[esHandle];
    unsigned head = atomic_load_explicit(&es->qHeadSeq, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&es->qTailSeq, memory_order_acquire);
    if (head == tail || !atomic_load_explicit(&es->acquired, memory_order_acquire))
        return (s32)CELL_DMUX_ERROR_EMPTY;

    free_entry(&es->queue[head % DMUX_AU_QUEUE_CAP]);
    atomic_store_explicit(&es->acquired, 0, memory_order_release);
    atomic_store_explicit(&es->qHeadSeq, head + 1u, memory_order_release);
    return CELL_OK;
}

s32 cellDmuxFlushEs(CellDmuxEsHandle esHandle)
{
    printf("[cellDmux] FlushEs(es=%u)\n", esHandle);
    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    DmuxEsSlot* es = &s_es[esHandle];
    if ((u8)es->filterId.filterIdMajor >= 0xE0u && (u8)es->filterId.filterIdMajor <= 0xEFu) {
        s32 rc = finalize_video_au(esHandle);
        if (rc != CELL_OK) return rc;
    }
    notify_es(es, esHandle, CELL_DMUX_ES_MSG_TYPE_FLUSH_DONE, 0);
    return CELL_OK;
}
