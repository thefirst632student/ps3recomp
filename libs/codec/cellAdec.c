/*
 * ps3recomp - cellAdec HLE implementation
 *
 * Audio decoder HLE. ATRAC3+ access units are decoded through FFmpeg when
 * available; callback/guest-buffer semantics remain usable on builds without
 * FFmpeg through the existing silence fallback.
 */

#include "cellAdec.h"
#include <stdio.h>
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../../runtime/ppu/ppu_context.h" /* g_active_ctx -> the guest lr */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */
#include "ps3emu/guest_call.h" /* g_ps3_guest_caller -- cbFunc is a GUEST OPD */
#include "../../runtime/memory/vm.h"     /* VM_HLE_INJECT_BASE */
#include "../audio/cellAudio.h"

#ifdef PS3RECOMP_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#endif

/* CellAdecPcmItem as the guest reads it, from RPCS3 Modules/cellAdec.h:339.
 * ps1_netemu copies the whole thing out with six 8-byte loads from offsets
 * 0x00..0x28 (at 0x000ED964), and that copy is what confirms these offsets
 * rather than a guess about padding: auInfo lands at 0x18, not 0x14, because it
 * contains a u64. The block must be 0x30 readable GUEST bytes. */
#define PCMITEM_PCM_HANDLE   0x00u
#define PCMITEM_STATUS       0x04u
#define PCMITEM_START_ADDR   0x08u
#define PCMITEM_SIZE         0x0Cu
#define PCMITEM_BSI_INFO     0x10u
#define PCMITEM_AU_START     0x18u
#define PCMITEM_AU_SIZE      0x1Cu
#define PCMITEM_AU_PTS_HI    0x20u
#define PCMITEM_AU_PTS_LO    0x24u
#define PCMITEM_AU_USERDATA  0x28u
#define PCMITEM_BYTES        0x30u

/* ATRAC3plus outputs 2048 samples per access unit.  WA2 opens codecType 13
 * (CELL_ADEC_TYPE_ATRACX_2CH), 48 kHz stereo float output, so one PCM block is
 * 2048 * 2 * sizeof(float) == 0x4000 bytes.  The old 1024-sample stub advanced
 * the movie audio clock at half speed and made VDISP hold all four VPOST
 * buffers waiting for video PTS to become due. */
#define ADEC_PCM_SAMPLES  2048u
#define ADEC_PCM_BYTES    (ADEC_PCM_SAMPLES * 2u * 4u)

/* Guest-visible scratch, in the HLE inject window rather than from the guest's
 * own heap. Taking 8 KB out of that heap in cellAdecOpen stalled the title
 * before it even reached cellAdecStartSeq -- ps1_netemu allocates its own
 * buffers from the same bump allocator, and perturbing it is not worth it for
 * two fixed-size blocks whose lifetime is the whole process.
 *
 * +0x40000 is clear of everything else in the window: labels +0x0000, control
 * +0x2000, callback +0x2F00, the offset tables +0x3000/+0x5000, and sys_rsx's
 * device/driver-info/reports pages at +0x30000/+0x31000/+0x38000. */
#define ADEC_SCRATCH_BASE   (VM_HLE_INJECT_BASE + 0x40000u)
#define ADEC_SCRATCH_STRIDE 0x8000u
#define ADEC_ITEM_EA(h)     (ADEC_SCRATCH_BASE + (u32)(h) * ADEC_SCRATCH_STRIDE)
#define ADEC_PCM_EA(h)      (ADEC_ITEM_EA(h) + 0x100u)

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_ADEC 4

typedef struct {
    int in_use;
    u32 codecType;
    u32 cbFunc;         /* guest EA of the callback's OPD */
    u32 cbArg;          /* guest EA handed back to it     */
    int seqStarted;
    u32 itemEa;         /* guest EA of the 0x30-byte CellAdecPcmItem */
    u32 pcmEa;          /* guest EA of the PCM buffer it points at    */
    int hasPcm;
    u32 pcmBytes;
    u32 pcmSamples;
    u32 pcmChannels;
    u32 pcmSampleRate;
    u32 auCount;        /* total AUs decoded */
#ifdef PS3RECOMP_HAVE_FFMPEG
    const AVCodec* codec;
    AVCodecContext* ctx;
    AVFrame* frame;
    u32 atracPayloadBytes;
    int atracConfigured;
    int skipFirstFrame;
#endif
#ifdef _WIN32
    CRITICAL_SECTION pcmMutex;
    CONDITION_VARIABLE pcmCond;
#else
    pthread_mutex_t pcmMutex;
    pthread_cond_t pcmCond;
#endif
    int pcmSyncInit;
} AdecSlot;

static AdecSlot s_adec[MAX_ADEC];

/* The real ATRAC-X decoder has one output surface and waits until the consumer
 * releases it before decoding the next AU (see RPCS3 cellAtracXdec.cpp,
 * "Waiting for output to be consumed").  This matters to WA2 because its
 * PCMOUT callback is edge/condition based, not an unbounded event counter.
 * A single hasPcm flag without backpressure loses PCMOUT notifications and,
 * worse, lets later AU metadata overwrite the PTS of the PCM still pending. */
static void pcm_sync_init(AdecSlot* a)
{
    if (a->pcmSyncInit) return;
#ifdef _WIN32
    InitializeCriticalSection(&a->pcmMutex);
    InitializeConditionVariable(&a->pcmCond);
#else
    pthread_mutex_init(&a->pcmMutex, NULL);
    pthread_cond_init(&a->pcmCond, NULL);
#endif
    a->pcmSyncInit = 1;
}

static void pcm_sync_destroy(AdecSlot* a)
{
    if (!a->pcmSyncInit) return;
#ifdef _WIN32
    DeleteCriticalSection(&a->pcmMutex);
#else
    pthread_cond_destroy(&a->pcmCond);
    pthread_mutex_destroy(&a->pcmMutex);
#endif
    a->pcmSyncInit = 0;
}

static void pcm_lock(AdecSlot* a)
{
#ifdef _WIN32
    EnterCriticalSection(&a->pcmMutex);
#else
    pthread_mutex_lock(&a->pcmMutex);
#endif
}

static void pcm_unlock(AdecSlot* a)
{
#ifdef _WIN32
    LeaveCriticalSection(&a->pcmMutex);
#else
    pthread_mutex_unlock(&a->pcmMutex);
#endif
}

static void pcm_wake_all(AdecSlot* a)
{
#ifdef _WIN32
    WakeAllConditionVariable(&a->pcmCond);
#else
    pthread_cond_broadcast(&a->pcmCond);
#endif
}

static void pcm_wait_10ms(AdecSlot* a)
{
#ifdef _WIN32
    SleepConditionVariableCS(&a->pcmCond, &a->pcmMutex, 10);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 10 * 1000 * 1000;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&a->pcmCond, &a->pcmMutex, &ts);
#endif
}

/* Fire one guest callback.
 *
 * The guest's cbFunc is a guest EA naming an OPD, so it goes through the
 * recompiled code's dispatcher. It used to be cast to a host function pointer
 * and called directly, which jumped to the guest address as host code:
 *
 *     [CRASH] code=0xC0000005 rip=00000000001B5F00
 *     [CRASH] last HLE NID (cellAdecDecodeAu)
 *
 * 0x1B5F00 is the guest OPD. That crash is what this function exists to stop.
 *
 * ponytail: dispatched SYNCHRONOUSLY on the caller's thread rather than from a
 * decoder thread as lv2 does. The ceiling is a guest callback that blocks
 * waiting on the thread that called DecodeAu -- give this its own thread if a
 * title ever deadlocks here. */
static void adec_notify(CellAdecHandle handle, u32 msg_type, s32 msg_data)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use) return;
    const AdecSlot* a = &s_adec[handle];
    { static int _n = 0;
      if (_n++ < 4)
          printf("[cellAdec] notify h=%u msg=%u data=0x%X cb=0x%08X arg=0x%08X caller=%d\n",
                 handle, msg_type, (unsigned)msg_data, a->cbFunc, a->cbArg,
                 g_ps3_guest_caller ? 1 : 0); }
    if (!a->cbFunc || !g_ps3_guest_caller) return;
    g_ps3_guest_caller(a->cbFunc, (u64)handle, (u64)msg_type,
                       (u64)(s64)msg_data, (u64)a->cbArg, 0, 0, 0, 0);
}

#ifdef PS3RECOMP_HAVE_FFMPEG
static void adec_ffmpeg_reset(AdecSlot* a)
{
    if (a->ctx) avcodec_free_context(&a->ctx);
    if (a->frame) av_frame_free(&a->frame);
    a->codec = NULL;
    a->atracPayloadBytes = 0;
    a->atracConfigured = 0;
}

static int adec_configure_atrac_from_ats(AdecSlot* a, const u8* src, u32 size)
{
    if (!src || size < 8 || src[0] != 0x0f || src[1] != 0xd0)
        return AVERROR_INVALIDDATA;

    u16 params = (u16)(((u16)src[2] << 8) | src[3]);
    u32 sr_idx = params >> 13;
    u32 ch_cfg = (params >> 10) & 7u;
    u32 payload = ((params & 0x03ffu) + 1u) * 8u;
    u32 rate = sr_idx == 1u ? 44100u : (sr_idx == 2u ? 48000u : 0u);
    u32 channels = ch_cfg <= 4u ? ch_cfg : ch_cfg + 1u;
    if (!rate || !channels || payload == 0 || size < payload + 8u)
        return AVERROR_INVALIDDATA;

    a->codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    if (!a->codec) return AVERROR_DECODER_NOT_FOUND;
    a->ctx = avcodec_alloc_context3(a->codec);
    a->frame = av_frame_alloc();
    if (!a->ctx || !a->frame) {
        adec_ffmpeg_reset(a);
        return AVERROR(ENOMEM);
    }
    a->ctx->block_align = (int)payload;
    a->ctx->sample_rate = (int)rate;
    a->ctx->ch_layout.nb_channels = (int)channels;
    int ret = avcodec_open2(a->ctx, a->codec, NULL);
    if (ret < 0) {
        adec_ffmpeg_reset(a);
        return ret;
    }
    a->atracPayloadBytes = payload;
    a->atracConfigured = 1;
    a->skipFirstFrame = 1; /* CELL ADEC ATRAC-X replaces first frame with silence. */
    fprintf(stderr,
            "[cellAdec] FFmpeg ATRAC3+ configured: %u Hz ch=%u payload=%u bytes\n",
            rate, channels, payload);
    return 0;
}

static int adec_decode_atrac(AdecSlot* a, const u8* src, u32 size,
                             float* host_pcm, u32 host_capacity_frames,
                             u32* frames_out, u32* channels_out, u32* rate_out)
{
    *frames_out = *channels_out = *rate_out = 0;
    if (!a->atracConfigured) {
        int r = adec_configure_atrac_from_ats(a, src, size);
        if (r < 0) return r;
    }
    if (size < 8u + a->atracPayloadBytes) return AVERROR_INVALIDDATA;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return AVERROR(ENOMEM);
    int ret = av_new_packet(pkt, (int)a->atracPayloadBytes);
    if (ret < 0) { av_packet_free(&pkt); return ret; }
    memcpy(pkt->data, src + 8u, a->atracPayloadBytes);

    ret = avcodec_send_packet(a->ctx, pkt);
    av_packet_free(&pkt);
    if (ret < 0) return ret;

    av_frame_unref(a->frame);
    ret = avcodec_receive_frame(a->ctx, a->frame);
    if (ret < 0) return ret;

    u32 channels = (u32)a->frame->ch_layout.nb_channels;
    if (!channels) channels = (u32)a->ctx->ch_layout.nb_channels;
    u32 frames = (u32)a->frame->nb_samples;
    u32 rate = (u32)(a->frame->sample_rate ? a->frame->sample_rate : a->ctx->sample_rate);
    if (!channels || !frames || frames > host_capacity_frames)
        return AVERROR_INVALIDDATA;

    if (a->skipFirstFrame) {
        memset(host_pcm, 0, (size_t)frames * channels * sizeof(float));
        a->skipFirstFrame = 0;
    } else if (a->frame->format == AV_SAMPLE_FMT_FLTP) {
        for (u32 i = 0; i < frames; ++i)
            for (u32 ch = 0; ch < channels; ++ch)
                host_pcm[i * channels + ch] = ((const float*)a->frame->extended_data[ch])[i];
    } else if (a->frame->format == AV_SAMPLE_FMT_FLT) {
        memcpy(host_pcm, a->frame->data[0], (size_t)frames * channels * sizeof(float));
    } else {
        fprintf(stderr, "[cellAdec] unsupported FFmpeg sample_fmt=%d\n", a->frame->format);
        return AVERROR_INVALIDDATA;
    }

    *frames_out = frames;
    *channels_out = channels;
    *rate_out = rate;
    return 0;
}
#endif

static void write_guest_float_be(u32 ea, float f)
{
    u32 bits;
    memcpy(&bits, &f, sizeof(bits));
    vm_write32(ea, bits);
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAdecQueryAttr(const CellAdecType* type, CellAdecAttr* attr)
{
    u32 codec_type = type ? vm_read32(GUEST_EA(type)) : 0;
    printf("[cellAdec] QueryAttr(codecType=%u)\n", codec_type);

    if (!type || !attr)
        return (s32)CELL_ADEC_ERROR_ARG;

    u32 ea = GUEST_EA(attr);
    vm_write32(ea + (u32)offsetof(CellAdecAttr, memSize), 64 * 1024);
    vm_write32(ea + (u32)offsetof(CellAdecAttr, decoderVerUpper), 1);
    vm_write32(ea + (u32)offsetof(CellAdecAttr, decoderVerLower), 0);
    return CELL_OK;
}

/* cellAdecOpen(type, res, cb, handle) -- FOUR arguments.
 *
 * This took five, splitting the guest's CellAdecCb struct into cbFunc + cbArg,
 * which pushed `handle` off r6 onto r7. r7 held whatever happened to be there, so
 * the null check failed and every Open returned CELL_ADEC_ERROR_ARG. ps1_netemu
 * opens a decoder for CD-DA/XA audio inside its CD-ROM constructor and gives up on
 * the error -- so the disc was never mounted and the emulator sat in its run loop
 * with nothing to run. Signature confirmed against RPCS3 (Modules/cellAdec.cpp).
 *
 * cb is a guest pointer to { u32 cbFunc; u32 cbArg; }. */
s32 cellAdecOpen(const CellAdecType* type, const CellAdecResource* res,
                 const CellAdecCb* cb, CellAdecHandle* handle)
{
    (void)res;

    u32 codec_type = type ? vm_read32(GUEST_EA(type)) : 0;   /* audioCodecType */
    u32 cb_ea      = (u32)(uintptr_t)cb;
    u32 handle_ea  = (u32)(uintptr_t)handle;
    printf("[cellAdec] Open(codecType=%u, cb=0x%08X, handle=0x%08X)\n",
           codec_type, cb_ea, handle_ea);

    if (!type || !handle_ea)
        return (s32)CELL_ADEC_ERROR_ARG;

    for (int i = 0; i < MAX_ADEC; i++) {
        if (!s_adec[i].in_use) {
            memset(&s_adec[i], 0, sizeof(AdecSlot));
            s_adec[i].in_use    = 1;
            s_adec[i].codecType = codec_type;
            pcm_sync_init(&s_adec[i]);
            if (cb_ea) {
                s_adec[i].cbFunc = vm_read32(cb_ea + 0);
                s_adec[i].cbArg  = vm_read32(cb_ea + 4);
            }
            /* The PcmItem is handed to the guest BY POINTER, so it cannot live
             * in host memory -- returning &host_struct is what crashed here
             * once already. */
            s_adec[i].itemEa = ADEC_ITEM_EA(i);
            s_adec[i].pcmEa  = ADEC_PCM_EA(i);
            vm_write32(handle_ea, (u32)i);
            printf("[cellAdec] Open -> handle=%u item=0x%08X pcm=0x%08X\n",
                   i, s_adec[i].itemEa, s_adec[i].pcmEa);
            return CELL_OK;
        }
    }
    return (s32)CELL_ADEC_ERROR_BUSY;
}

s32 cellAdecClose(CellAdecHandle handle)
{
    printf("[cellAdec] Close(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    AdecSlot* a = &s_adec[handle];
#ifdef PS3RECOMP_HAVE_FFMPEG
    adec_ffmpeg_reset(a);
#endif
    if (a->pcmSyncInit) {
        pcm_lock(a);
        a->in_use = 0;
        a->hasPcm = 0;
        pcm_wake_all(a);
        pcm_unlock(a);
        pcm_sync_destroy(a);
    } else {
        a->in_use = 0;
    }
    return CELL_OK;
}

s32 cellAdecStartSeq(CellAdecHandle handle, void* param)
{
    (void)param;
    printf("[cellAdec] StartSeq(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    AdecSlot* a = &s_adec[handle];
    pcm_lock(a);
    a->seqStarted = 1;
    a->hasPcm = 0;
    a->pcmBytes = 0;
    a->pcmSamples = 0;
    a->pcmChannels = 0;
    a->pcmSampleRate = 0;
    a->auCount = 0;
#ifdef PS3RECOMP_HAVE_FFMPEG
    if (a->ctx) avcodec_flush_buffers(a->ctx);
    a->skipFirstFrame = 1;
#endif
    pcm_wake_all(a);
    pcm_unlock(a);
    return CELL_OK;
}

s32 cellAdecEndSeq(CellAdecHandle handle)
{
    /* ADEC_WHO=1: name the guest function driving this. The title calls EndSeq
     * hundreds of times against a single StartSeq, and the image has eight
     * EndSeq call sites -- the backtrace says which one. */
    { static int _n = -1;
      if (_n < 0) _n = getenv("ADEC_WHO") ? 0 : -2;
      if (_n >= 0 && _n < 8) { _n++;
          /* The guest lr, not a host backtrace. ppu_guest_caller maps host
           * frames to the nearest lifted function, and here it landed on
           * func_00013040 -- a single `blr`, so plainly a mis-attribution.
           * lr is written by the `bl` to the import stub, so it is exactly
           * (call site + 4) for a call like this one. */
          extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
          printf("[cellAdec] EndSeq from guest lr=0x%08X tid=%llu\n",
                 g_active_ctx ? (u32)g_active_ctx->lr : 0u,
                 g_active_ctx ? (unsigned long long)g_active_ctx->thread_id : 0ull); } }
    printf("[cellAdec] EndSeq(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    {
        AdecSlot* a = &s_adec[handle];
        pcm_lock(a);
        a->seqStarted = 0;
        pcm_wake_all(a);
        pcm_unlock(a);
    }

    adec_notify(handle, CELL_ADEC_MSG_TYPE_SEQDONE, CELL_OK);

    return CELL_OK;
}

s32 cellAdecDecodeAu(CellAdecHandle handle, const CellAdecAuInfo* auInfo)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;
    if (!auInfo)
        return (s32)CELL_ADEC_ERROR_ARG;

    AdecSlot* a = &s_adec[handle];

    /* One decoded PCM frame may be outstanding at a time.  Without this wait,
     * DecodeAu overwrites itemEa (including its AU PTS) while apostThread is
     * still consuming the previous frame. */
    pcm_lock(a);
    while (a->hasPcm && a->in_use && a->seqStarted) {
        static int wait_logs = 0;
        if (wait_logs++ < 12)
            printf("[cellAdec] output busy; waiting for PCM consumer\n");
        pcm_wait_10ms(a);
    }
    if (!a->in_use) {
        pcm_unlock(a);
        return (s32)CELL_ADEC_ERROR_ARG;
    }
    if (!a->seqStarted) {
        pcm_unlock(a);
        return (s32)CELL_ADEC_ERROR_SEQ;
    }
    pcm_unlock(a);

    printf("[cellAdec] DecodeAu(handle=%u, addr=0x%X, size=%u, pts=%llu)\n",
           handle, vm_read32(GUEST_EA(auInfo) + (u32)offsetof(CellAdecAuInfo, startAddr)),
           vm_read32(GUEST_EA(auInfo) + (u32)offsetof(CellAdecAuInfo, size)),
           (unsigned long long)vm_read64(GUEST_EA(auInfo) + 0x08u));

    /* Step 1: report the AU consumed. msgData is the AU INFO ADDRESS, not a
     * status -- see RPCS3 Modules/cellAdec.cpp:1345. A decoder that tracks its
     * outstanding AUs by address gets a null back if this is CELL_OK. */
    adec_notify(handle, CELL_ADEC_MSG_TYPE_AUDONE, (s32)GUEST_EA(auInfo));

    /* Step 2: decode ATRAC3+ into real float PCM when FFmpeg is present. */
    a->auCount++;
    {
        const u32 au = GUEST_EA(auInfo);
        const u32 it = a->itemEa;
        const u32 start_addr = vm_read32(au + (u32)offsetof(CellAdecAuInfo, startAddr));
        const u32 au_size = vm_read32(au + (u32)offsetof(CellAdecAuInfo, size));
        u32 pcm_frames = ADEC_PCM_SAMPLES;
        u32 pcm_channels = 2;
        u32 pcm_rate = 48000;
        u32 pcm_bytes = ADEC_PCM_BYTES;
        int decoded_real = 0;

#ifdef PS3RECOMP_HAVE_FFMPEG
        if (start_addr && au_size >= 8u) {
            /* ATRAC3+ is at most 8 channels; 2048 samples => 64 KiB temp. */
            float host_pcm[ADEC_PCM_SAMPLES * 8u];
            u32 got_frames = 0, got_channels = 0, got_rate = 0;
            const u8* src = GUEST_PTR((void*)(uintptr_t)start_addr, const u8*);
            int dr = adec_decode_atrac(a, src, au_size, host_pcm,
                                       ADEC_PCM_SAMPLES,
                                       &got_frames, &got_channels, &got_rate);
            if (dr >= 0 && got_frames && got_channels) {
                /* WA2 opens the 2-channel ATRAC-X codec.  Keep the guest-facing
                 * block stereo even if a future stream advertises more channels;
                 * the direct host fallback also uses the first stereo pair. */
                pcm_frames = got_frames;
                pcm_channels = got_channels >= 2u ? 2u : 1u;
                pcm_rate = got_rate;
                pcm_bytes = pcm_frames * pcm_channels * 4u;
                if (pcm_bytes > ADEC_PCM_BYTES) pcm_bytes = ADEC_PCM_BYTES;
                u32 samples_to_write = pcm_bytes / 4u;
                for (u32 i = 0; i < samples_to_write; ++i) {
                    u32 frame = i / pcm_channels;
                    u32 ch = i % pcm_channels;
                    float f = host_pcm[frame * got_channels + ch];
                    write_guest_float_be(a->pcmEa + i * 4u, f);
                }
                for (u32 o = pcm_bytes; o < ADEC_PCM_BYTES; o += 4u)
                    vm_write32(a->pcmEa + o, 0);

                /* Until the persistent synth2 mixer is faithfully resident,
                 * cellAudio's HLE fallback is the only path that can reach the
                 * real host sink.  It auto-disables mixing if guest ports ever
                 * start carrying real PCM. */
                cellAudioHlePcmSubmitF32(host_pcm, got_frames, got_channels, got_rate);
                decoded_real = 1;
            } else {
                char err[128] = {0};
                av_strerror(dr, err, sizeof(err));
                static int decode_logs = 0;
                if (decode_logs++ < 12)
                    fprintf(stderr, "[cellAdec] FFmpeg ATRAC decode failed: %s (%d), AU=%u bytes\n",
                            err, dr, au_size);
            }
        }
#endif
        if (!decoded_real) {
            for (u32 o = 0; o < ADEC_PCM_BYTES; o += 4u) vm_write32(a->pcmEa + o, 0);
        }

        a->pcmBytes = pcm_bytes;
        a->pcmSamples = pcm_frames;
        a->pcmChannels = pcm_channels;
        a->pcmSampleRate = pcm_rate;

        vm_write32(it + PCMITEM_PCM_HANDLE, (u32)handle);
        vm_write32(it + PCMITEM_STATUS,     0);              /* CELL_OK */
        vm_write32(it + PCMITEM_START_ADDR, a->pcmEa);
        vm_write32(it + PCMITEM_SIZE,       pcm_bytes);
        vm_write32(it + PCMITEM_BSI_INFO,   0);
        vm_write32(it + PCMITEM_AU_START,   start_addr);
        vm_write32(it + PCMITEM_AU_SIZE,    au_size);
        vm_write32(it + PCMITEM_AU_PTS_HI,       vm_read32(au + 0x08));
        vm_write32(it + PCMITEM_AU_PTS_LO,       vm_read32(au + 0x0C));
        vm_write32(it + PCMITEM_AU_USERDATA,     vm_read32(au + 0x10));
        vm_write32(it + PCMITEM_AU_USERDATA + 4, vm_read32(au + 0x14));
    }
    pcm_lock(a);
    a->hasPcm = 1;
    pcm_unlock(a);

    adec_notify(handle, CELL_ADEC_MSG_TYPE_PCMOUT, CELL_OK);

    return CELL_OK;
}

s32 cellAdecGetPcm(CellAdecHandle handle, void* outBuffer)
{
    { static int _n = 0; if (_n++ < 6)
        printf("[cellAdec] GetPcm(handle=%u out=0x%08X)\n",
               handle, GUEST_EA(outBuffer)); }
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    AdecSlot* a = &s_adec[handle];
    pcm_lock(a);
    if (!a->hasPcm) {
        pcm_unlock(a);
        return (s32)CELL_ADEC_ERROR_EMPTY;
    }

    /* Preserve the exact big-endian float bytes generated above. */
    { const u32 out = GUEST_EA(outBuffer);
      const u32 bytes = a->pcmBytes ? a->pcmBytes : ADEC_PCM_BYTES;
      if (out) memcpy(GUEST_PTR((void*)(uintptr_t)out, void*),
                      GUEST_PTR((void*)(uintptr_t)a->pcmEa, const void*), bytes); }

    a->hasPcm = 0;
    pcm_wake_all(a);
    pcm_unlock(a);
    return CELL_OK;
}

s32 cellAdecGetPcmItem(CellAdecHandle handle, const CellAdecPcmItem** pcmItem)
{
    { static int _n = 0; if (_n++ < 12) {
        const int hp = handle < MAX_ADEC ? s_adec[handle].hasPcm : -1;
        const u64 pts = (handle < MAX_ADEC && hp) ? vm_read64(s_adec[handle].itemEa + PCMITEM_AU_PTS_HI) : 0;
        printf("[cellAdec] GetPcmItem(handle=%u out=0x%08X hasPcm=%d pts=%llu pcmBytes=0x%X)\n",
               handle, GUEST_EA(pcmItem), hp, (unsigned long long)pts,
               (handle < MAX_ADEC && s_adec[handle].pcmBytes) ? s_adec[handle].pcmBytes : ADEC_PCM_BYTES);
      } }
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    AdecSlot* a = &s_adec[handle];
    pcm_lock(a);
    if (!a->hasPcm) {
        pcm_unlock(a);
        return (s32)CELL_ADEC_ERROR_EMPTY;
    }

    /* pcmItem is a GUEST CellAdecPcmItem**. Writing &host_struct here put a
     * HOST address into guest memory; the guest dereferenced it and died on a
     * wild address (rip 0x870D0E, fault 0xCFFDFFF0). Write the guest EA. */
    if (pcmItem)
        vm_write32(GUEST_EA(pcmItem), a->itemEa);

    pcm_unlock(a);
    return CELL_OK;
}
