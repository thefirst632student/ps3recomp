/*
 * ps3recomp - cellAtrac (libatrac3plus) HLE implementation
 *
 * Provides HLE decoder lifecycle, stream buffer tracking, and audio decode
 * for games using the standalone cellAtrac library.
 */

#include "cellAtrac.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <stdio.h>
#include <string.h>

#define MAX_ATRAC_HANDLES 8

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
} AtracSlot;

static AtracSlot s_atrac_slots[MAX_ATRAC_HANDLES];

static AtracSlot* atrac_find_or_alloc(CellAtracHandle* handle)
{
    u32 ea = (u32)(uintptr_t)handle;
    if (!ea) return NULL;
    for (int i = 0; i < MAX_ATRAC_HANDLES; i++) {
        if (s_atrac_slots[i].in_use && s_atrac_slots[i].ea == ea)
            return &s_atrac_slots[i];
    }
    for (int i = 0; i < MAX_ATRAC_HANDLES; i++) {
        if (!s_atrac_slots[i].in_use) {
            s_atrac_slots[i].in_use = 1;
            s_atrac_slots[i].ea = ea;
            s_atrac_slots[i].is_decoder_created = 0;
            s_atrac_slots[i].loop_num = -1;
            s_atrac_slots[i].channels = 2;
            s_atrac_slots[i].sample_rate = 48000;
            s_atrac_slots[i].bitrate = 128000;
            s_atrac_slots[i].total_samples = 48000 * 300; /* 5 min */
            s_atrac_slots[i].current_sample = 0;
            return &s_atrac_slots[i];
        }
    }
    return NULL;
}

s32 cellAtracSetDataAndGetMemSize(CellAtracHandle* handle, void* pucBufferAddr,
                                  u32 uiReadByte, u32 uiBufferByte, u32* puiWorkMemSize)
{
    printf("[cellAtrac] SetDataAndGetMemSize(handle=%p, buf=%p, read=%u, bufSize=%u)\n",
           handle, pucBufferAddr, uiReadByte, uiBufferByte);
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (!slot) return (s32)CELL_ATRAC_ERROR_API_FAIL;

    slot->buffer_ea = (u32)(uintptr_t)pucBufferAddr;
    slot->buffer_size = uiBufferByte;
    if (puiWorkMemSize) {
        vm_write32((u32)(uintptr_t)puiWorkMemSize, 0x10000); /* 64KB work memory */
    }
    return CELL_OK;
}

s32 cellAtracCreateDecoder(CellAtracHandle* handle, void* pucWorkMem,
                           u32 uiPpuThreadPriority, u32 uiSpuThreadPriority)
{
    (void)uiPpuThreadPriority; (void)uiSpuThreadPriority;
    printf("[cellAtrac] CreateDecoder(handle=%p, work=%p)\n", handle, pucWorkMem);
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (!slot) return (s32)CELL_ATRAC_ERROR_API_FAIL;
    slot->is_decoder_created = 1;
    return CELL_OK;
}

s32 cellAtracCreateDecoderExt(CellAtracHandle* handle, void* pucWorkMem,
                              u32 uiWorkMemSize, CellAtracExtRes* pExtRes)
{
    (void)pExtRes;
    printf("[cellAtrac] CreateDecoderExt(handle=%p, work=%p, size=%u)\n",
           handle, pucWorkMem, uiWorkMemSize);
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (!slot) return (s32)CELL_ATRAC_ERROR_API_FAIL;
    slot->is_decoder_created = 1;
    return CELL_OK;
}

s32 cellAtracDeleteDecoder(CellAtracHandle* handle)
{
    printf("[cellAtrac] DeleteDecoder(handle=%p)\n", handle);
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (slot) {
        slot->in_use = 0;
        slot->is_decoder_created = 0;
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
    if (puiChannel) {
        vm_write32((u32)(uintptr_t)puiChannel, slot ? slot->channels : 2);
    }
    return CELL_OK;
}

s32 cellAtracGetRemainFrame(CellAtracHandle* handle, s32* piRemainFrame)
{
    (void)handle;
    if (piRemainFrame) {
        vm_write32((u32)(uintptr_t)piRemainFrame, (u32)CELL_ATRAC_ALLDATA_IS_ON_MEMORY);
    }
    return CELL_OK;
}

s32 cellAtracGetSoundInfo(CellAtracHandle* handle, s32* piEndSample,
                          s32* piLoopStartSample, s32* piLoopEndSample)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    s32 total = slot ? (s32)slot->total_samples : (48000 * 300);
    if (piEndSample) vm_write32((u32)(uintptr_t)piEndSample, (u32)total);
    if (piLoopStartSample) vm_write32((u32)(uintptr_t)piLoopStartSample, 0);
    if (piLoopEndSample) vm_write32((u32)(uintptr_t)piLoopEndSample, (u32)total);
    return CELL_OK;
}

s32 cellAtracDecode(CellAtracHandle* handle, float* pOutPcm, u32* puiSamples,
                    u32* puiFinishFlag, s32* piRemainFrame)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    u32 samplesPerFrame = 2048;
    if (pOutPcm) {
        memset(GUEST_PTR(pOutPcm, void*), 0, samplesPerFrame * 2 * sizeof(float));
    }
    if (puiSamples) {
        vm_write32((u32)(uintptr_t)puiSamples, samplesPerFrame);
    }
    if (puiFinishFlag) {
        vm_write32((u32)(uintptr_t)puiFinishFlag, 0);
    }
    if (piRemainFrame) {
        vm_write32((u32)(uintptr_t)piRemainFrame, (u32)CELL_ATRAC_ALLDATA_IS_ON_MEMORY);
    }
    if (slot) {
        slot->current_sample += samplesPerFrame;
    }
    return CELL_OK;
}

s32 cellAtracAddStreamData(CellAtracHandle* handle, u32 uiAddByte)
{
    (void)handle; (void)uiAddByte;
    return CELL_OK;
}

s32 cellAtracGetSecondBufferInfo(CellAtracHandle* handle, u32* puiReadPosition, u32* puiWritableByte)
{
    (void)handle;
    if (puiReadPosition) vm_write32((u32)(uintptr_t)puiReadPosition, 0);
    if (puiWritableByte) vm_write32((u32)(uintptr_t)puiWritableByte, 0x10000);
    return CELL_OK;
}

s32 cellAtracSetSecondBuffer(CellAtracHandle* handle, void* pucSecondBufferAddr, u32 uiSecondBufferByte)
{
    (void)handle; (void)pucSecondBufferAddr; (void)uiSecondBufferByte;
    return CELL_OK;
}

s32 cellAtracGetVacantSize(CellAtracHandle* handle, u32* puiVacantSize)
{
    (void)handle;
    if (puiVacantSize) vm_write32((u32)(uintptr_t)puiVacantSize, 0x10000);
    return CELL_OK;
}

s32 cellAtracGetStreamDataInfo(CellAtracHandle* handle, void** ppucWriteAddr,
                               u32* puiWritableByte, u32* puiReadPosition)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (ppucWriteAddr && slot) vm_write32((u32)(uintptr_t)ppucWriteAddr, slot->buffer_ea);
    if (puiWritableByte) vm_write32((u32)(uintptr_t)puiWritableByte, 0x10000);
    if (puiReadPosition) vm_write32((u32)(uintptr_t)puiReadPosition, 0);
    return CELL_OK;
}

s32 cellAtracGetMaxSample(CellAtracHandle* handle, u32* puiMaxSample)
{
    (void)handle;
    if (puiMaxSample) vm_write32((u32)(uintptr_t)puiMaxSample, 2048);
    return CELL_OK;
}

s32 cellAtracGetNextSample(CellAtracHandle* handle, u32* puiNextSample)
{
    (void)handle;
    if (puiNextSample) vm_write32((u32)(uintptr_t)puiNextSample, 2048);
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
    if (puiBitrate) vm_write32((u32)(uintptr_t)puiBitrate, slot ? slot->bitrate : 128000);
    return CELL_OK;
}

s32 cellAtracGetLoopInfo(CellAtracHandle* handle, s32* piLoopNum, u32* puiLoopStatus)
{
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (piLoopNum) vm_write32((u32)(uintptr_t)piLoopNum, slot ? (u32)slot->loop_num : (u32)-1);
    if (puiLoopStatus) vm_write32((u32)(uintptr_t)puiLoopStatus, 1);
    return CELL_OK;
}

s32 cellAtracIsSecondBufferNeeded(CellAtracHandle* handle)
{
    (void)handle;
    return 0; /* Not needed */
}

s32 cellAtracGetBufferInfoForResetting(CellAtracHandle* handle, u32 uiSample, CellAtracBufferInfo* pBufferInfo)
{
    (void)uiSample;
    AtracSlot* slot = atrac_find_or_alloc(handle);
    u32 ea = (u32)(uintptr_t)pBufferInfo;
    if (ea && slot) {
        vm_write32(ea + 0, slot->buffer_ea);
        vm_write32(ea + 4, 0x10000);
        vm_write32(ea + 8, 0);
        vm_write32(ea + 12, 0);
    }
    return CELL_OK;
}

s32 cellAtracResetPlayPosition(CellAtracHandle* handle, u32 uiSample, u32 uiWriteByte)
{
    (void)uiWriteByte;
    AtracSlot* slot = atrac_find_or_alloc(handle);
    if (slot) slot->current_sample = uiSample;
    return CELL_OK;
}

s32 cellAtracGetInternalErrorInfo(CellAtracHandle* handle, s32* piResult)
{
    (void)handle;
    if (piResult) vm_write32((u32)(uintptr_t)piResult, 0);
    return CELL_OK;
}
