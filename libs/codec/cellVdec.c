/*
 * ps3recomp - cellVdec HLE implementation
 *
 * Real AVC/MPEG-2 decode backend when FFmpeg is available.  cellDmux now
 * supplies elementary-stream access units; DecodeAu also keeps a compatibility
 * path that can extract video PES payload if an older caller sends PAMF /
 * MPEG-PS data directly.
 *
 * Semantics follow cellVdec/RPCS3: GetPicItem publishes metadata without
 * consuming the picture; GetPicture consumes the oldest decoded picture and
 * copies pixels to the caller's output buffer.
 */

#include "cellVdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include "../guest_struct.h"
#include "../../runtime/ppu/ppu_memory.h"

#ifdef PS3RECOMP_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#endif

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;

#define MAX_VDEC 4
#define VDEC_QUEUE_CAP 16
#define PICITEM_SIZE 0x80u
#define PICINFO_OFFSET 0x80u

typedef struct {
#ifdef PS3RECOMP_HAVE_FFMPEG
    AVFrame* frame;
#endif
    u32 width;
    u32 height;
    u64 pts;
    u64 dts;
    u64 userData;
    u32 auNum;
    int picItemReceived;
    int synthetic;
} VdecFrame;

typedef struct {
    int in_use;
    u32 codecType;
    u32 cbFunc;
    u32 cbArg;
    u32 resMemAddr;
    u32 resMemSize;
    int seqStarted;
    u32 auCount;
    u32 frameRateCode;
    VdecFrame queue[VDEC_QUEUE_CAP];
    int qHead;
    int qCount;
#ifdef PS3RECOMP_HAVE_FFMPEG
    const AVCodec* codec;
    AVCodecContext* ctx;
    struct SwsContext* sws;
#endif
} VdecSlot;

static VdecSlot s_vdec[MAX_VDEC];

static void callback(VdecSlot* v, u32 handle, u32 type, s32 status)
{
    if (v->cbFunc && g_ps3_guest_caller) {
        g_ps3_guest_caller(v->cbFunc, handle, type, (u32)status, v->cbArg,
                           0, 0, 0, 0);
    }
}

static void clear_queue(VdecSlot* v)
{
    for (int i = 0; i < VDEC_QUEUE_CAP; ++i) {
#ifdef PS3RECOMP_HAVE_FFMPEG
        if (v->queue[i].frame) av_frame_free(&v->queue[i].frame);
#endif
        memset(&v->queue[i], 0, sizeof(v->queue[i]));
    }
    v->qHead = 0;
    v->qCount = 0;
}

static int queue_synthetic(VdecSlot* v, const CellVdecAuInfo* au, u32 width, u32 height)
{
    if (v->qCount >= VDEC_QUEUE_CAP) return -1;
    int pos = (v->qHead + v->qCount) % VDEC_QUEUE_CAP;
    VdecFrame* q = &v->queue[pos];
    memset(q, 0, sizeof(*q));
    q->width = width ? width : 1280;
    q->height = height ? height : 720;
    q->pts = au->pts;
    q->dts = au->dts;
    q->userData = au->userData;
    q->auNum = v->auCount;
    q->synthetic = 1;
    v->qCount++;
    return 0;
}

#ifdef PS3RECOMP_HAVE_FFMPEG
static int ffmpeg_open(VdecSlot* v)
{
    enum AVCodecID id = AV_CODEC_ID_NONE;
    if (v->codecType == CELL_VDEC_CODEC_TYPE_AVC) id = AV_CODEC_ID_H264;
    else if (v->codecType == CELL_VDEC_CODEC_TYPE_MPEG2) id = AV_CODEC_ID_MPEG2VIDEO;
    else if (v->codecType == CELL_VDEC_CODEC_TYPE_DIVX) id = AV_CODEC_ID_MPEG4;
    if (id == AV_CODEC_ID_NONE) return -1;

    v->codec = avcodec_find_decoder(id);
    if (!v->codec) return -1;
    v->ctx = avcodec_alloc_context3(v->codec);
    if (!v->ctx) return -1;
    v->ctx->pkt_timebase = (AVRational){1, 90000};
    if (avcodec_open2(v->ctx, v->codec, NULL) < 0) {
        avcodec_free_context(&v->ctx);
        return -1;
    }
    return 0;
}

static void ffmpeg_close(VdecSlot* v)
{
    clear_queue(v);
    if (v->ctx) avcodec_free_context(&v->ctx);
    if (v->sws) sws_freeContext(v->sws);
    v->sws = NULL;
    v->codec = NULL;
}

static int looks_like_ps(const u8* p, size_t n)
{
    size_t lim = n < 4096 ? n : 4096;
    for (size_t i = 0; i + 4 <= lim; ++i) {
        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1 &&
            (p[i+3] == 0xBA || p[i+3] == 0xBB || (p[i+3] >= 0xE0 && p[i+3] <= 0xEF)))
            return 1;
    }
    return 0;
}

/* Extract MPEG-2 PES video payload (stream_id 0xE0..0xEF) from PAMF's
 * program stream.  The returned packet is Annex-B H.264/MPEG-2 ES data.
 */
static u8* extract_video_es(const u8* src, size_t srcSize, size_t* outSize)
{
    *outSize = 0;
    if (!looks_like_ps(src, srcSize)) return NULL;

    u8* out = (u8*)av_malloc(srcSize + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!out) return NULL;
    size_t wr = 0;

    for (size_t i = 0; i + 6 <= srcSize;) {
        if (!(src[i] == 0 && src[i+1] == 0 && src[i+2] == 1)) { ++i; continue; }
        u8 sid = src[i+3];
        if (sid < 0xE0 || sid > 0xEF) { i += 4; continue; }

        size_t pesLen = ((size_t)src[i+4] << 8) | src[i+5];
        size_t packetEnd = pesLen ? i + 6 + pesLen : srcSize;
        if (packetEnd > srcSize) packetEnd = srcSize;
        if (i + 9 > packetEnd) { i = packetEnd > i ? packetEnd : i + 4; continue; }

        size_t payload = i + 9 + src[i+8];
        if (payload > packetEnd) { i = packetEnd; continue; }
        size_t bytes = packetEnd - payload;
        if (bytes) { memcpy(out + wr, src + payload, bytes); wr += bytes; }
        i = packetEnd > i ? packetEnd : i + 4;
    }

    if (!wr) { av_free(out); return NULL; }
    memset(out + wr, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    *outSize = wr;
    return out;
}

static int queue_frame(VdecSlot* v, AVFrame* f, const CellVdecAuInfo* au)
{
    if (v->qCount >= VDEC_QUEUE_CAP) return -1;
    int pos = (v->qHead + v->qCount) % VDEC_QUEUE_CAP;
    VdecFrame* q = &v->queue[pos];
    memset(q, 0, sizeof(*q));
    q->frame = f;
    q->width = (u32)f->width;
    q->height = (u32)f->height;
    q->pts = f->pts != AV_NOPTS_VALUE ? (u64)f->pts : au->pts;
    q->dts = f->pkt_dts != AV_NOPTS_VALUE ? (u64)f->pkt_dts : au->dts;
    q->userData = au->userData;
    q->auNum = v->auCount;
    v->qCount++;
    return 0;
}

static int decode_packet(VdecSlot* v, const u8* data, size_t size, const CellVdecAuInfo* au)
{
    if (size > INT_MAX) return AVERROR(EINVAL);

    /* libavcodec bitstream readers are allowed to read
     * AV_INPUT_BUFFER_PADDING_SIZE bytes beyond pkt->size.  Guest AU memory has
     * no such contract, so never point AVPacket directly into vm_base: copy it
     * into an FFmpeg-owned packet whose padding is allocated/zeroed correctly. */
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return AVERROR(ENOMEM);
    int ret = av_new_packet(pkt, (int)size);
    if (ret < 0) { av_packet_free(&pkt); return ret; }
    memcpy(pkt->data, data, size);
    pkt->pts = au->pts == UINT64_MAX ? AV_NOPTS_VALUE : (int64_t)au->pts;
    pkt->dts = au->dts == UINT64_MAX ? AV_NOPTS_VALUE : (int64_t)au->dts;

    ret = avcodec_send_packet(v->ctx, pkt);
    av_packet_free(&pkt);
    if (ret < 0) return ret;

    for (;;) {
        AVFrame* f = av_frame_alloc();
        if (!f) return AVERROR(ENOMEM);
        ret = avcodec_receive_frame(v->ctx, f);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) { av_frame_free(&f); return 0; }
        if (ret < 0) { av_frame_free(&f); return ret; }
        if (queue_frame(v, f, au) < 0) { av_frame_free(&f); return AVERROR(ENOBUFS); }
    }
}
#endif

s32 cellVdecQueryAttr(const CellVdecType* type, CellVdecAttr* attr)
{
    if (!type || !attr) return (s32)CELL_VDEC_ERROR_ARG;
    u32 ea = GUEST_EA(attr);
    vm_write32(ea + (u32)offsetof(CellVdecAttr, memSize), 4 * 1024 * 1024);
    vm_write8(ea + (u32)offsetof(CellVdecAttr, cmdDepth), 4);
    vm_write32(ea + (u32)offsetof(CellVdecAttr, decoderVerUpper), 1);
    vm_write32(ea + (u32)offsetof(CellVdecAttr, decoderVerLower), 0);
    return CELL_OK;
}

s32 cellVdecQueryAttrEx(const CellVdecType* type, CellVdecAttr* attr) { return cellVdecQueryAttr(type, attr); }

s32 cellVdecOpen(const CellVdecType* type, const CellVdecResource* res,
                 const CellVdecCb* cb, CellVdecHandle* handle)
{
    if (!type || !handle) return (s32)CELL_VDEC_ERROR_ARG;
    u32 codec = vm_read32(GUEST_EA(type) + offsetof(CellVdecType, codecType));
    for (int i = 0; i < MAX_VDEC; ++i) if (!s_vdec[i].in_use) {
        VdecSlot* v = &s_vdec[i];
        memset(v, 0, sizeof(*v));
        v->in_use = 1; v->codecType = codec;
        if (cb) { v->cbFunc = vm_read32(GUEST_EA(cb)); v->cbArg = vm_read32(GUEST_EA(cb) + 4); }
        if (res) { v->resMemAddr = vm_read32(GUEST_EA(res)); v->resMemSize = vm_read32(GUEST_EA(res) + 4); }
#ifdef PS3RECOMP_HAVE_FFMPEG
        if (ffmpeg_open(v) < 0) { memset(v, 0, sizeof(*v)); return (s32)CELL_VDEC_ERROR_FATAL; }
#else
        printf("[cellVdec] FFmpeg backend not built; real video decode unavailable\n");
#endif
        vm_write32(GUEST_EA(handle), (u32)i);
        printf("[cellVdec] Open -> handle=%d codec=%u FFmpeg=%s\n", i, codec,
#ifdef PS3RECOMP_HAVE_FFMPEG
               "yes"
#else
               "no"
#endif
        );
        return CELL_OK;
    }
    return (s32)CELL_VDEC_ERROR_BUSY;
}

s32 cellVdecOpenEx(const CellVdecType* type, const CellVdecResource* res,
                   const CellVdecCb* cb, CellVdecHandle* handle) { return cellVdecOpen(type, res, cb, handle); }

s32 cellVdecClose(CellVdecHandle handle)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use) return (s32)CELL_VDEC_ERROR_ARG;
#ifdef PS3RECOMP_HAVE_FFMPEG
    ffmpeg_close(&s_vdec[handle]);
#else
    clear_queue(&s_vdec[handle]);
#endif
    memset(&s_vdec[handle], 0, sizeof(s_vdec[handle]));
    return CELL_OK;
}

s32 cellVdecStartSeq(CellVdecHandle handle)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use) return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];
    v->seqStarted = 1; v->auCount = 0;
    clear_queue(v);
#ifdef PS3RECOMP_HAVE_FFMPEG
    avcodec_flush_buffers(v->ctx);
#endif
    return CELL_OK;
}

s32 cellVdecEndSeq(CellVdecHandle handle)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use) return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];
#ifdef PS3RECOMP_HAVE_FFMPEG
    if (v->ctx) {
        avcodec_send_packet(v->ctx, NULL);
        CellVdecAuInfo dummy = {0};
        dummy.pts = dummy.dts = UINT64_MAX;
        for (;;) {
            AVFrame* f = av_frame_alloc();
            if (!f) break;
            int r = avcodec_receive_frame(v->ctx, f);
            if (r < 0) { av_frame_free(&f); break; }
            if (queue_frame(v, f, &dummy) < 0) { av_frame_free(&f); break; }
            callback(v, handle, CELL_VDEC_MSG_TYPE_PICOUT, CELL_OK);
        }
    }
#endif
    v->seqStarted = 0;
    callback(v, handle, CELL_VDEC_MSG_TYPE_SEQDONE, CELL_OK);
    return CELL_OK;
}

s32 cellVdecDecodeAu(CellVdecHandle handle, s32 mode, const CellVdecAuInfo* auInfo)
{
    (void)mode;
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use || !auInfo) return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];
    if (!v->seqStarted) return (s32)CELL_VDEC_ERROR_SEQ;

    u32 ea = GUEST_EA(auInfo);
    CellVdecAuInfo au;
    au.startAddr = vm_read32(ea + offsetof(CellVdecAuInfo, startAddr));
    au.size = vm_read32(ea + offsetof(CellVdecAuInfo, size));
    au.pts = vm_read64(ea + offsetof(CellVdecAuInfo, pts));
    au.dts = vm_read64(ea + offsetof(CellVdecAuInfo, dts));
    au.userData = vm_read64(ea + offsetof(CellVdecAuInfo, userData));
    if (!au.startAddr || !au.size) return (s32)CELL_VDEC_ERROR_ARG;
    v->auCount++;

#ifdef PS3RECOMP_HAVE_FFMPEG
    const u8* src = GUEST_PTR((void*)(uintptr_t)au.startAddr, const u8*);
    size_t esSize = 0;
    u8* extracted = extract_video_es(src, au.size, &esSize);
    const u8* packet = extracted ? extracted : src;
    size_t packetSize = extracted ? esSize : au.size;
    int before = v->qCount;
    int ret = decode_packet(v, packet, packetSize, &au);
    if (extracted) av_free(extracted);
    callback(v, handle, CELL_VDEC_MSG_TYPE_AUDONE, ret < 0 ? CELL_VDEC_ERROR_AU : CELL_OK);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        printf("[cellVdec] decode failed: %s (%d), input=%s size=%zu\n", err, ret,
               extracted ? "PAMF/PES" : "ES", packetSize);
        return (s32)CELL_VDEC_ERROR_AU;
    }
    for (int i = before; i < v->qCount; ++i) callback(v, handle, CELL_VDEC_MSG_TYPE_PICOUT, CELL_OK);
#else
    /* Keep cellVdec callback semantics alive even on builds where libavcodec
     * was not detected.  The old no-FFmpeg path emitted AUDONE only, so the
     * movie display thread waited forever for PICOUT.  A neutral black frame
     * is preferable to deadlocking the title; the log makes this fallback
     * explicit so it cannot be mistaken for real decoding. */
    if (queue_synthetic(v, &au, 1280, 720) < 0) {
        callback(v, handle, CELL_VDEC_MSG_TYPE_AUDONE, CELL_VDEC_ERROR_BUSY);
        return (s32)CELL_VDEC_ERROR_BUSY;
    }
    printf("[cellVdec] DecodeAu #%u: FFmpeg=no, synthetic PICOUT 1280x720 (addr=0x%08X size=%u)\n",
           v->auCount, au.startAddr, au.size);
    callback(v, handle, CELL_VDEC_MSG_TYPE_AUDONE, CELL_OK);
    callback(v, handle, CELL_VDEC_MSG_TYPE_PICOUT, CELL_OK);
#endif
    return CELL_OK;
}

static void write_pic_item(VdecSlot* v, VdecFrame* q, u32 item)
{
    u32 info = item + PICINFO_OFFSET;
    u32 width = q->width ? q->width : 1280;
    u32 height = q->height ? q->height : 720;
    u32 bufSize = width * height * 3 / 2;
#ifdef PS3RECOMP_HAVE_FFMPEG
    if (q->frame && !q->synthetic) {
        int realSize = av_image_get_buffer_size((enum AVPixelFormat)q->frame->format,
                                                q->frame->width, q->frame->height, 1);
        if (realSize > 0) bufSize = (u32)realSize;
    }
#endif

    vm_write32(item + 0x00, v->codecType);
    vm_write32(item + 0x04, 0x00000123);
    vm_write32(item + 0x08, (u32)((bufSize + 127) & ~127));
    vm_write8 (item + 0x0c, 1);
    vm_write32(item + 0x10, (u32)(q->pts >> 32)); vm_write32(item + 0x14, (u32)q->pts);
    vm_write32(item + 0x18, 0xffffffff); vm_write32(item + 0x1c, 0xffffffff);
    vm_write32(item + 0x20, (u32)(q->dts >> 32)); vm_write32(item + 0x24, (u32)q->dts);
    vm_write32(item + 0x28, 0xffffffff); vm_write32(item + 0x2c, 0xffffffff);
    vm_write64(item + 0x30, q->userData); vm_write64(item + 0x38, 0);
    vm_write32(item + 0x40, CELL_OK); vm_write32(item + 0x44, 0);
    vm_write32(item + 0x48, info);

    for (u32 off = 0; off < 0x180; off += 4) vm_write32(info + off, 0);
    /* AVC/MPEG2 info both begin with horizontalSize/verticalSize in the SDK. */
    vm_write16(info + 0x00, (u16)width);
    vm_write16(info + 0x02, (u16)height);
}

s32 cellVdecGetPicItem(CellVdecHandle handle, void* picItem)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use || !picItem) return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];
    for (int n = 0; n < v->qCount; ++n) {
        int idx = (v->qHead + n) % VDEC_QUEUE_CAP;
        VdecFrame* q = &v->queue[idx];
        if (!q->picItemReceived) {
            u32 item = v->resMemAddr ? v->resMemAddr : 0x40000000;
            write_pic_item(v, q, item);
            q->picItemReceived = 1;
            vm_write32(GUEST_EA(picItem), item);
            printf("[cellVdec] GetPicItem -> item=0x%08X frame=%ux%u synthetic=%d qCount=%d\n",
                   item, q->width, q->height, q->synthetic, v->qCount);
            return CELL_OK;
        }
    }
    return (s32)CELL_VDEC_ERROR_EMPTY;
}

s32 cellVdecGetPicture(CellVdecHandle handle, const CellVdecPicFormat* format, void* outBuff)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use || !format) return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];
    if (!v->qCount) return (s32)CELL_VDEC_ERROR_EMPTY;
    VdecFrame* q = &v->queue[v->qHead];
    u32 width = q->width ? q->width : 1280;
    u32 height = q->height ? q->height : 720;
    u32 fmtEa = GUEST_EA(format);
    u32 fmt = vm_read32(fmtEa + 0);

    if (outBuff) {
        u8* base = GUEST_PTR(outBuff, u8*);
#ifdef PS3RECOMP_HAVE_FFMPEG
        if (q->frame && !q->synthetic) {
            AVFrame* f = q->frame;
            enum AVPixelFormat dstFmt;
            int stride;
            switch (fmt) {
                case CELL_VDEC_PICFMT_ARGB32_ILV: dstFmt = AV_PIX_FMT_ARGB; stride = f->width * 4; break;
                case CELL_VDEC_PICFMT_RGBA32_ILV: dstFmt = AV_PIX_FMT_RGBA; stride = f->width * 4; break;
                case CELL_VDEC_PICFMT_UYVY422_ILV: dstFmt = AV_PIX_FMT_UYVY422; stride = f->width * 2; break;
                case CELL_VDEC_PICFMT_YUV420_PLANAR: dstFmt = AV_PIX_FMT_YUV420P; stride = f->width; break;
                default: return (s32)CELL_VDEC_ERROR_ARG;
            }
            v->sws = sws_getCachedContext(v->sws, f->width, f->height, (enum AVPixelFormat)f->format,
                                          f->width, f->height, dstFmt, SWS_POINT, NULL, NULL, NULL);
            if (!v->sws) return (s32)CELL_VDEC_ERROR_FATAL;
            u8* dst[4] = { base, NULL, NULL, NULL };
            int lines[4] = { stride, 0, 0, 0 };
            if (dstFmt == AV_PIX_FMT_YUV420P) {
                dst[1] = base + f->width * f->height;
                dst[2] = dst[1] + (f->width / 2) * (f->height / 2);
                lines[1] = lines[2] = f->width / 2;
            }
            sws_scale(v->sws, (const u8* const*)f->data, f->linesize, 0, f->height, dst, lines);
        } else
#endif
        {
            /* Synthetic fallback: write a standards-shaped neutral black frame
             * in the exact format requested by the game/vpost pipeline. */
            u8 alpha = vm_read8(fmtEa + offsetof(CellVdecPicFormat, alpha));
            switch (fmt) {
                case CELL_VDEC_PICFMT_ARGB32_ILV:
                    for (u32 i = 0, pixels = width * height; i < pixels; ++i) {
                        base[i * 4 + 0] = alpha;
                        base[i * 4 + 1] = 0; base[i * 4 + 2] = 0; base[i * 4 + 3] = 0;
                    }
                    break;
                case CELL_VDEC_PICFMT_RGBA32_ILV:
                    for (u32 i = 0, pixels = width * height; i < pixels; ++i) {
                        base[i * 4 + 0] = 0; base[i * 4 + 1] = 0; base[i * 4 + 2] = 0; base[i * 4 + 3] = alpha;
                    }
                    break;
                case CELL_VDEC_PICFMT_UYVY422_ILV:
                    for (u32 i = 0, pairs = width * height / 2; i < pairs; ++i) {
                        base[i * 4 + 0] = 128; base[i * 4 + 1] = 16;
                        base[i * 4 + 2] = 128; base[i * 4 + 3] = 16;
                    }
                    break;
                case CELL_VDEC_PICFMT_YUV420_PLANAR: {
                    size_t ysz = (size_t)width * height;
                    size_t csz = (size_t)(width / 2) * (height / 2);
                    memset(base, 16, ysz);
                    memset(base + ysz, 128, csz * 2);
                    break;
                }
                default:
                    return (s32)CELL_VDEC_ERROR_ARG;
            }
        }
    }

    printf("[cellVdec] GetPicture <- frame=%ux%u fmt=%u synthetic=%d qCount=%d\n",
           width, height, fmt, q->synthetic, v->qCount);
#ifdef PS3RECOMP_HAVE_FFMPEG
    if (q->frame) av_frame_free(&q->frame);
#endif
    memset(q, 0, sizeof(*q));
    v->qHead = (v->qHead + 1) % VDEC_QUEUE_CAP;
    v->qCount--;
    return CELL_OK;
}

s32 cellVdecSetFrameRate(CellVdecHandle handle, u32 frameRateCode)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use) return (s32)CELL_VDEC_ERROR_ARG;
    s_vdec[handle].frameRateCode = frameRateCode;
    return CELL_OK;
}

int cellVdec_is_seq_active(void) { return s_vdec[0].in_use && s_vdec[0].seqStarted; }
