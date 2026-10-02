/*
 * ps3recomp - cellAtrac (libatrac3plus) HLE implementation
 *
 * Standalone ATRAC3+ decoder.  Supports the two layouts commonly handed to
 * cellAtrac by PS3 titles without pulling libavformat into the runtime:
 *   - RIFF/WAVE ATRAC3+ (fixed block_align payloads)
 *   - raw Sony ATS frames (8-byte 0x0fd0 header + ATRAC3+ payload)
 *
 * When FFmpeg is unavailable the lifecycle remains functional and Decode
 * falls back to silence, matching the old implementation.
 */

#include "cellAtrac.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../audio/cellAudio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PS3RECOMP_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#endif

#define MAX_ATRAC_HANDLES 8
#define ATRAC_SAMPLES_PER_FRAME 2048u
#define ATRAC_MAX_CHANNELS 8u

typedef struct {
    u32 ea;
    int in_use;
    int is_decoder_created;
    s32 loop_num;
    u32 channels;
    u32 sample_rate;
    u32 bitrate;
    u32 total_samples;
    u32 current_sample;
    u32 buffer_ea;
    u32 buffer_size;
    u32 read_bytes;
    u32 data_offset;
    u32 data_size;
    u32 cursor;
    u32 frame_bytes;       /* payload bytes for RIFF, total bytes for ATS */
    int ats_mode;
#ifdef PS3RECOMP_HAVE_FFMPEG
    const AVCodec* codec;
    AVCodecContext* ctx;
    AVFrame* frame;
#endif
} AtracSlot;

static AtracSlot s_atrac_slots[MAX_ATRAC_HANDLES];

static u16 rd_le16(const u8* p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u32 rd_le32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd_be16(const u8* p) { return (u16)(((u16)p[0] << 8) | p[1]); }

static void write_guest_float(u32 ea, float f)
{
    u32 bits;
    memcpy(&bits, &f, 4);
    vm_write32(ea, bits);
}

static AtracSlot* atrac_find_or_alloc(CellAtracHandle* handle)
{
    u32 ea = (u32)(uintptr_t)handle;
    if (!ea) return NULL;
    for (int i = 0; i < MAX_ATRAC_HANDLES; i++)
        if (s_atrac_slots[i].in_use && s_atrac_slots[i].ea == ea)
            return &s_atrac_slots[i];
    for (int i = 0; i < MAX_ATRAC_HANDLES; i++) {
        if (!s_atrac_slots[i].in_use) {
            memset(&s_atrac_slots[i], 0, sizeof(s_atrac_slots[i]));
            s_atrac_slots[i].in_use = 1;
            s_atrac_slots[i].ea = ea;
            s_atrac_slots[i].loop_num = -1;
            s_atrac_slots[i].channels = 2;
            s_atrac_slots[i].sample_rate = 48000;
            s_atrac_slots[i].bitrate = 128000;
            return &s_atrac_slots[i];
        }
    }
    return NULL;
}

#ifdef PS3RECOMP_HAVE_FFMPEG
static void atrac_ffmpeg_close(AtracSlot* s)
{
    if (s->ctx) avcodec_free_context(&s->ctx);
    if (s->frame) av_frame_free(&s->frame);
    s->codec = NULL;
}

static int atrac_ffmpeg_open(AtracSlot* s, u32 payload_bytes)
{
    if (!payload_bytes || !s->channels || !s->sample_rate) return AVERROR_INVALIDDATA;
    atrac_ffmpeg_close(s);
    s->codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    if (!s->codec) return AVERROR_DECODER_NOT_FOUND;
    s->ctx = avcodec_alloc_context3(s->codec);
    s->frame = av_frame_alloc();
    if (!s->ctx || !s->frame) { atrac_ffmpeg_close(s); return AVERROR(ENOMEM); }
    s->ctx->block_align = (int)payload_bytes;
    s->ctx->sample_rate = (int)s->sample_rate;
    s->ctx->ch_layout.nb_channels = (int)s->channels;
    int r = avcodec_open2(s->ctx, s->codec, NULL);
    if (r < 0) { atrac_ffmpeg_close(s); return r; }
    fprintf(stderr, "[cellAtrac] FFmpeg ATRAC3+ ready: %u Hz ch=%u payload=%u mode=%s\n",
            s->sample_rate, s->channels, payload_bytes, s->ats_mode ? "ATS" : "RIFF");
    return 0;
}
#endif

static int atrac_parse_ats(AtracSlot* s, const u8* p, u32 bytes, u32* payload_out)
{
    if (!p || bytes < 8 || rd_be16(p) != 0x0fd0) return -1;
    u16 params = rd_be16(p + 2);
    u32 sr_idx = params >> 13;
    u32 ch_cfg = (params >> 10) & 7u;
    u32 payload = ((params & 0x03ffu) + 1u) * 8u;
    u32 rate = sr_idx == 1u ? 44100u : (sr_idx == 2u ? 48000u : 0u);
    u32 channels = ch_cfg <= 4u ? ch_cfg : ch_cfg + 1u;
    if (!rate || !channels || payload + 8u > bytes) return -1;
    s->sample_rate = rate;
    s->channels = channels;
    s->ats_mode = 1;
    s->frame_bytes = payload + 8u;
    if (payload_out) *payload_out = payload;
    return 0;
}

static int atrac_parse_riff(AtracSlot* s, const u8* p, u32 bytes)
{
    if (!p || bytes < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return -1;
    u32 off = 12;
    u32 block_align = 0, data_off = 0, data_size = 0;
    while (off + 8u <= bytes) {
        const u8* h = p + off;
        u32 n = rd_le32(h + 4);
        u32 body = off + 8u;
        if (body > bytes || n > bytes - body) break;
        if (!memcmp(h, "fmt ", 4) && n >= 16u) {
            s->channels = rd_le16(p + body + 2u);
            s->sample_rate = rd_le32(p + body + 4u);
            block_align = rd_le16(p + body + 12u);
        } else if (!memcmp(h, "data", 4)) {
            data_off = body;
            data_size = n;
            break;
        }
        off = body + ((n + 1u) & ~1u);
    }
    if (!block_align || !data_off || !data_size || !s->channels || !s->sample_rate) return -1;
    if (data_off + data_size > bytes) data_size = bytes - data_off;
    s->ats_mode = 0;
    s->frame_bytes = block_align;
    s->data_offset = data_off;
    s->data_size = data_size;
    return 0;
}

static void atrac_log_head_once(const u8* p, u32 n)
{
    static int once = 0;
    if (once++ || !p) return;
    u32 lim = n < 32u ? n : 32u;
    fprintf(stderr, "[cellAtrac] input head (%u bytes):", lim);
    for (u32 i = 0; i < lim; ++i) fprintf(stderr, " %02X", p[i]);
    fprintf(stderr, "\n");
}

s32 cellAtracSetDataAndGetMemSize(CellAtracHandle* handle, void* pucBufferAddr,
                                  u32 uiReadByte, u32 uiBufferByte, u32* puiWorkMemSize)
{
    printf("[cellAtrac] SetDataAndGetMemSize(handle=%p, buf=%p, read=%u, bufSize=%u)\n",
           handle, pucBufferAddr, uiReadByte, uiBufferByte);
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (!slot) return (s32)CELL_ATRAC_ERROR_API_FAIL;

#ifdef PS3RECOMP_HAVE_FFMPEG
    atrac_ffmpeg_close(slot);
#endif
    slot->buffer_ea = (u32)(uintptr_t)pucBufferAddr;
    slot->buffer_size = uiBufferByte;
    slot->read_bytes = uiReadByte;
    slot->data_offset = 0;
    slot->data_size = uiReadByte;
    slot->cursor = 0;
    slot->current_sample = 0;
    slot->frame_bytes = 0;
    slot->ats_mode = 0;

    if (slot->buffer_ea && uiReadByte) {
        const u8* p = GUEST_PTR((void*)(uintptr_t)slot->buffer_ea, const u8*);
        atrac_log_head_once(p, uiReadByte);
        u32 payload = 0;
        if (atrac_parse_riff(slot, p, uiReadByte) == 0) {
            fprintf(stderr, "[cellAtrac] parsed RIFF: dataOff=%u data=%u block=%u %uHz ch=%u\n",
                    slot->data_offset, slot->data_size, slot->frame_bytes,
                    slot->sample_rate, slot->channels);
        } else if (atrac_parse_ats(slot, p, uiReadByte, &payload) == 0) {
            slot->data_offset = 0;
            slot->data_size = uiReadByte;
            fprintf(stderr, "[cellAtrac] parsed raw ATS: frame=%u payload=%u %uHz ch=%u\n",
                    slot->frame_bytes, payload, slot->sample_rate, slot->channels);
        } else {
            fprintf(stderr, "[cellAtrac] format not yet recognized; decode will remain silent for this stream\n");
        }
    }

    if (slot->frame_bytes)
        slot->total_samples = (slot->data_size / slot->frame_bytes) * ATRAC_SAMPLES_PER_FRAME;
    else
        slot->total_samples = 0;

    if (puiWorkMemSize) vm_write32((u32)(uintptr_t)puiWorkMemSize, 0x10000);
    return CELL_OK;
}

static s32 atrac_create_decoder_common(AtracSlot* slot)
{
    if (!slot) return (s32)CELL_ATRAC_ERROR_API_FAIL;
    slot->is_decoder_created = 1;
#ifdef PS3RECOMP_HAVE_FFMPEG
    if (slot->frame_bytes && slot->buffer_ea) {
        u32 payload = slot->frame_bytes;
        if (slot->ats_mode) payload -= 8u;
        int r = atrac_ffmpeg_open(slot, payload);
        if (r < 0) {
            char err[128] = {0}; av_strerror(r, err, sizeof(err));
            fprintf(stderr, "[cellAtrac] FFmpeg open failed: %s (%d)\n", err, r);
        }
    }
#endif
    return CELL_OK;
}

s32 cellAtracCreateDecoder(CellAtracHandle* handle, void* pucWorkMem,
                           u32 uiPpuThreadPriority, u32 uiSpuThreadPriority)
{
    (void)uiPpuThreadPriority; (void)uiSpuThreadPriority;
    printf("[cellAtrac] CreateDecoder(handle=%p, work=%p)\n", handle, pucWorkMem);
    return atrac_create_decoder_common(atrac_find_or_alloc(handle));
}

s32 cellAtracCreateDecoderExt(CellAtracHandle* handle, void* pucWorkMem,
                              u32 uiWorkMemSize, CellAtracExtRes* pExtRes)
{
    (void)pExtRes;
    printf("[cellAtrac] CreateDecoderExt(handle=%p, work=%p, size=%u)\n",
           handle, pucWorkMem, uiWorkMemSize);
    return atrac_create_decoder_common(atrac_find_or_alloc(handle));
}

s32 cellAtracDeleteDecoder(CellAtracHandle* handle)
{
    printf("[cellAtrac] DeleteDecoder(handle=%p)\n", handle);
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (slot) {
#ifdef PS3RECOMP_HAVE_FFMPEG
        atrac_ffmpeg_close(slot);
#endif
        memset(slot, 0, sizeof(*slot));
    }
    return CELL_OK;
}

s32 cellAtracSetLoopNum(CellAtracHandle* handle, s32 nLoopNum)
{
    printf("[cellAtrac] SetLoopNum(handle=%p, loopNum=%d)\n", handle, nLoopNum);
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (slot) slot->loop_num = nLoopNum;
    return CELL_OK;
}

s32 cellAtracGetChannel(CellAtracHandle* handle, u32* puiChannel)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (puiChannel) vm_write32((u32)(uintptr_t)puiChannel, slot ? slot->channels : 2);
    return CELL_OK;
}

static s32 atrac_remaining_frames(const AtracSlot* slot)
{
    if (!slot || !slot->frame_bytes || slot->cursor >= slot->data_size) return 0;
    return (s32)((slot->data_size - slot->cursor) / slot->frame_bytes);
}

s32 cellAtracGetRemainFrame(CellAtracHandle* handle, s32* piRemainFrame)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (piRemainFrame)
        vm_write32((u32)(uintptr_t)piRemainFrame,
                   slot && slot->frame_bytes ? (u32)atrac_remaining_frames(slot)
                                             : (u32)CELL_ATRAC_ALLDATA_IS_ON_MEMORY);
    return CELL_OK;
}

s32 cellAtracGetSoundInfo(CellAtracHandle* handle, s32* piEndSample,
                          s32* piLoopStartSample, s32* piLoopEndSample)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    s32 total = slot ? (s32)slot->total_samples : 0;
    if (piEndSample) vm_write32((u32)(uintptr_t)piEndSample, (u32)total);
    if (piLoopStartSample) vm_write32((u32)(uintptr_t)piLoopStartSample, 0);
    if (piLoopEndSample) vm_write32((u32)(uintptr_t)piLoopEndSample, (u32)total);
    return CELL_OK;
}

s32 cellAtracDecode(CellAtracHandle* handle, float* pOutPcm, u32* puiSamples,
                    u32* puiFinishFlag, s32* piRemainFrame)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (!slot || !slot->is_decoder_created) return (s32)CELL_ATRAC_ERROR_NO_DECODER;

    if (!slot->frame_bytes || !slot->buffer_ea) {
        if (pOutPcm) memset(GUEST_PTR(pOutPcm, void*), 0, ATRAC_SAMPLES_PER_FRAME * 2u * sizeof(float));
        if (puiSamples) vm_write32((u32)(uintptr_t)puiSamples, ATRAC_SAMPLES_PER_FRAME);
        if (puiFinishFlag) vm_write32((u32)(uintptr_t)puiFinishFlag, 0);
        if (piRemainFrame) vm_write32((u32)(uintptr_t)piRemainFrame, (u32)CELL_ATRAC_ALLDATA_IS_ON_MEMORY);
        return CELL_OK;
    }

    if (slot->cursor + slot->frame_bytes > slot->data_size) {
        if (slot->loop_num != 0) {
            if (slot->loop_num > 0) slot->loop_num--;
            slot->cursor = 0;
            slot->current_sample = 0;
#ifdef PS3RECOMP_HAVE_FFMPEG
            if (slot->ctx) avcodec_flush_buffers(slot->ctx);
#endif
        } else {
            if (puiSamples) vm_write32((u32)(uintptr_t)puiSamples, 0);
            if (puiFinishFlag) vm_write32((u32)(uintptr_t)puiFinishFlag, 1);
            if (piRemainFrame) vm_write32((u32)(uintptr_t)piRemainFrame, 0);
            return CELL_OK;
        }
    }

    u32 out_frames = ATRAC_SAMPLES_PER_FRAME;
    u32 out_channels = slot->channels ? slot->channels : 2u;
    float pcm[ATRAC_SAMPLES_PER_FRAME * ATRAC_MAX_CHANNELS];
    memset(pcm, 0, sizeof(pcm));
    int real = 0;

#ifdef PS3RECOMP_HAVE_FFMPEG
    if (slot->ctx && slot->frame) {
        const u8* base = GUEST_PTR((void*)(uintptr_t)slot->buffer_ea, const u8*);
        const u8* framep = base + slot->data_offset + slot->cursor;
        const u8* payload = framep;
        u32 payload_bytes = slot->frame_bytes;
        if (slot->ats_mode) {
            u32 parsed_payload = 0;
            if (atrac_parse_ats(slot, framep, slot->data_size - slot->cursor, &parsed_payload) == 0) {
                payload = framep + 8u;
                payload_bytes = parsed_payload;
            }
        }

        AVPacket* pkt = av_packet_alloc();
        int r = pkt ? av_new_packet(pkt, (int)payload_bytes) : AVERROR(ENOMEM);
        if (r >= 0) {
            memcpy(pkt->data, payload, payload_bytes);
            r = avcodec_send_packet(slot->ctx, pkt);
        }
        if (pkt) av_packet_free(&pkt);
        if (r >= 0) {
            av_frame_unref(slot->frame);
            r = avcodec_receive_frame(slot->ctx, slot->frame);
        }
        if (r >= 0) {
            u32 n = (u32)slot->frame->nb_samples;
            u32 ch = (u32)slot->frame->ch_layout.nb_channels;
            if (!ch) ch = slot->channels;
            if (n <= ATRAC_SAMPLES_PER_FRAME && ch && ch <= ATRAC_MAX_CHANNELS) {
                if (slot->frame->format == AV_SAMPLE_FMT_FLTP) {
                    for (u32 i = 0; i < n; ++i)
                        for (u32 c = 0; c < ch; ++c)
                            pcm[i * ch + c] = ((const float*)slot->frame->extended_data[c])[i];
                    real = 1;
                } else if (slot->frame->format == AV_SAMPLE_FMT_FLT) {
                    memcpy(pcm, slot->frame->data[0], (size_t)n * ch * sizeof(float));
                    real = 1;
                }
                if (real) { out_frames = n; out_channels = ch; }
            }
        }
        if (r < 0) {
            static int dl = 0;
            if (dl++ < 8) { char err[128] = {0}; av_strerror(r, err, sizeof(err));
                fprintf(stderr, "[cellAtrac] decode failed at byte %u: %s (%d)\n", slot->cursor, err, r); }
        }
    }
#endif

    if (pOutPcm) {
        u32 out = (u32)(uintptr_t)pOutPcm;
        /* libatrac3plus callers in this title use stereo float output. */
        u32 out_ch = out_channels >= 2u ? 2u : 1u;
        for (u32 i = 0; i < out_frames; ++i)
            for (u32 c = 0; c < out_ch; ++c)
                write_guest_float(out + (i * out_ch + c) * 4u, pcm[i * out_channels + c]);
    }
    if (real)
        cellAudioHlePcmSubmitF32(pcm, out_frames, out_channels, slot->sample_rate);

    slot->cursor += slot->frame_bytes;
    slot->current_sample += out_frames;
    if (puiSamples) vm_write32((u32)(uintptr_t)puiSamples, out_frames);
    if (puiFinishFlag) vm_write32((u32)(uintptr_t)puiFinishFlag, 0);
    if (piRemainFrame) vm_write32((u32)(uintptr_t)piRemainFrame, (u32)atrac_remaining_frames(slot));
    return CELL_OK;
}

s32 cellAtracAddStreamData(CellAtracHandle* handle, u32 uiAddByte)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (slot) {
        u64 n = (u64)slot->read_bytes + uiAddByte;
        slot->read_bytes = (u32)(n > slot->buffer_size ? slot->buffer_size : n);
        if (slot->data_offset + slot->data_size < slot->read_bytes)
            slot->data_size = slot->read_bytes - slot->data_offset;
    }
    return CELL_OK;
}

s32 cellAtracGetSecondBufferInfo(CellAtracHandle* handle, u32* puiReadPosition, u32* puiWritableByte)
{
    (void)handle;
    if (puiReadPosition) vm_write32((u32)(uintptr_t)puiReadPosition, 0);
    if (puiWritableByte) vm_write32((u32)(uintptr_t)puiWritableByte, 0);
    return CELL_OK;
}

s32 cellAtracSetSecondBuffer(CellAtracHandle* handle, void* pucSecondBufferAddr, u32 uiSecondBufferByte)
{
    (void)handle; (void)pucSecondBufferAddr; (void)uiSecondBufferByte;
    return CELL_OK;
}

s32 cellAtracGetVacantSize(CellAtracHandle* handle, u32* puiVacantSize)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (puiVacantSize) vm_write32((u32)(uintptr_t)puiVacantSize,
        slot && slot->buffer_size > slot->read_bytes ? slot->buffer_size - slot->read_bytes : 0);
    return CELL_OK;
}

s32 cellAtracGetStreamDataInfo(CellAtracHandle* handle, void** ppucWriteAddr,
                               u32* puiWritableByte, u32* puiReadPosition)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (ppucWriteAddr && slot) vm_write32((u32)(uintptr_t)ppucWriteAddr, slot->buffer_ea + slot->read_bytes);
    if (puiWritableByte) vm_write32((u32)(uintptr_t)puiWritableByte,
        slot && slot->buffer_size > slot->read_bytes ? slot->buffer_size - slot->read_bytes : 0);
    if (puiReadPosition) vm_write32((u32)(uintptr_t)puiReadPosition, slot ? slot->read_bytes : 0);
    return CELL_OK;
}

s32 cellAtracGetMaxSample(CellAtracHandle* handle, u32* puiMaxSample)
{
    (void)handle;
    if (puiMaxSample) vm_write32((u32)(uintptr_t)puiMaxSample, ATRAC_SAMPLES_PER_FRAME);
    return CELL_OK;
}

s32 cellAtracGetNextSample(CellAtracHandle* handle, u32* puiNextSample)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (puiNextSample) vm_write32((u32)(uintptr_t)puiNextSample,
        slot && slot->cursor < slot->data_size ? ATRAC_SAMPLES_PER_FRAME : 0);
    return CELL_OK;
}

s32 cellAtracGetNextDecodePosition(CellAtracHandle* handle, u32* puiNextDecodePosition)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (puiNextDecodePosition) vm_write32((u32)(uintptr_t)puiNextDecodePosition, slot ? slot->current_sample : 0);
    return CELL_OK;
}

s32 cellAtracGetBitrate(CellAtracHandle* handle, u32* puiBitrate)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (puiBitrate && slot) {
        u32 b = slot->frame_bytes && slot->sample_rate
              ? (slot->frame_bytes * 8u * slot->sample_rate) / ATRAC_SAMPLES_PER_FRAME
              : slot->bitrate;
        vm_write32((u32)(uintptr_t)puiBitrate, b);
    }
    return CELL_OK;
}

s32 cellAtracGetLoopInfo(CellAtracHandle* handle, s32* piLoopNum, u32* puiLoopStatus)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (piLoopNum) vm_write32((u32)(uintptr_t)piLoopNum, slot ? (u32)slot->loop_num : 0);
    if (puiLoopStatus) vm_write32((u32)(uintptr_t)puiLoopStatus, slot && slot->loop_num != 0 ? 1u : 0u);
    return CELL_OK;
}

s32 cellAtracIsSecondBufferNeeded(CellAtracHandle* handle)
{
    (void)handle;
    return 0;
}

s32 cellAtracGetBufferInfoForResetting(CellAtracHandle* handle, u32 uiSample, CellAtracBufferInfo* pBufferInfo)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    u32 ea = (u32)(uintptr_t)pBufferInfo;
    if (ea && slot) {
        u32 frame = uiSample / ATRAC_SAMPLES_PER_FRAME;
        u32 read_pos = frame * slot->frame_bytes;
        vm_write32(ea + 0, slot->buffer_ea + slot->data_offset + read_pos);
        vm_write32(ea + 4, slot->buffer_size > read_pos ? slot->buffer_size - read_pos : 0);
        vm_write32(ea + 8, 0);
        vm_write32(ea + 12, read_pos);
    }
    return CELL_OK;
}

s32 cellAtracResetPlayPosition(CellAtracHandle* handle, u32 uiSample, u32 uiWriteByte)
{
    (void)uiWriteByte;
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (slot) {
        slot->current_sample = uiSample;
        slot->cursor = (uiSample / ATRAC_SAMPLES_PER_FRAME) * slot->frame_bytes;
        if (slot->cursor > slot->data_size) slot->cursor = slot->data_size;
#ifdef PS3RECOMP_HAVE_FFMPEG
        if (slot->ctx) avcodec_flush_buffers(slot->ctx);
#endif
    }
    return CELL_OK;
}

s32 cellAtracGetInternalErrorInfo(CellAtracHandle* handle, s32* piResult)
{
    (void)handle;
    if (piResult) vm_write32((u32)(uintptr_t)piResult, 0);
    return CELL_OK;
}
