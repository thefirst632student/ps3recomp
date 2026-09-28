#include <cstdio>
#include <cstring>
#include <cstdint>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

// For SPURS workload registry
#include "spu_context.h"
#include "spu_workload.h"


extern "C" void ps3_hle_register(unsigned int nid, const char* name, void* handler);
extern "C" s32 cellSpursEventFlagSet(void* eventFlag, u16 bits);

extern "C" {
    s32 cellRescGcmSurface2RescSrc(const void* surface, void* src);
    s32 cellRescSetWaitFlip(void* context);
    s32 cellRescVideoOutResolutionId2RescBufferMode(u32 resolutionId, u32* bufferMode);
    s32 cellAudioSetPortLevel(u32 portNum, float level);
    s32 _sys_spu_printf_initialize(const char* name, int prio);
    s32 _cellSpursLFQueuePushBody(u64 queue_ea, u64 buffer_ea, u32 is_blocking);
    s32 cellSpursLFQueueDetachLv2EventQueue(u64 queue_ea);

    s32 cellPamfGetHeaderSize(const void* pamfAddr, u64 fileSize, u64* headerSize);
    s32 cellPamfGetStreamOffsetAndSize(const void* pamfAddr, u64 fileSize, u64* streamOffset, u64* streamSize);
    s32 cellPamfReaderSetStreamWithTypeAndIndex(void* reader, u8 streamType, u32 streamIndex);
    s32 cellPamfReaderGetStreamTypeAndChannel(void* reader, u8* pStreamType, u8* pCh);

    s32 cellAtracSetDataAndGetMemSize(void* handle, void* pucBufferAddr, u32 uiReadByte, u32 uiBufferByte, u32* puiWorkMemSize);
    s32 cellAtracCreateDecoder(void* handle, void* pucWorkMem, u32 uiPpuThreadPriority, u32 uiSpuThreadPriority);
    s32 cellAtracCreateDecoderExt(void* handle, void* pucWorkMem, u32 uiWorkMemSize, void* pExtRes);
    s32 cellAtracDeleteDecoder(void* handle);
    s32 cellAtracDecode(void* handle, float* pOutPcm, u32* puiSamples, u32* puiFinishFlag, s32* piRemainFrame);
    s32 cellAtracAddStreamData(void* handle, u32 uiAddByte);
    s32 cellAtracGetSecondBufferInfo(void* handle, u32* puiReadPosition, u32* puiWritableByte);
    s32 cellAtracSetSecondBuffer(void* handle, void* pucSecondBufferAddr, u32 uiSecondBufferByte);
    s32 cellAtracGetRemainFrame(void* handle, s32* piRemainFrame);
    s32 cellAtracGetVacantSize(void* handle, u32* puiVacantSize);
    s32 cellAtracGetStreamDataInfo(void* handle, void** ppucWriteAddr, u32* puiWritableByte, u32* puiReadPosition);
    s32 cellAtracGetChannel(void* handle, u32* puiChannel);
    s32 cellAtracGetMaxSample(void* handle, u32* puiMaxSample);
    s32 cellAtracGetNextSample(void* handle, u32* puiNextSample);
    s32 cellAtracGetSoundInfo(void* handle, s32* piEndSample, s32* piLoopStartSample, s32* piLoopEndSample);
    s32 cellAtracGetNextDecodePosition(void* handle, u32* puiNextDecodePosition);
    s32 cellAtracGetBitrate(void* handle, u32* puiBitrate);
    s32 cellAtracGetLoopInfo(void* handle, s32* piLoopNum, u32* puiLoopStatus);
    s32 cellAtracIsSecondBufferNeeded(void* handle);
    s32 cellAtracSetLoopNum(void* handle, s32 nLoopNum);
    s32 cellAtracGetBufferInfoForResetting(void* handle, u32 uiSample, void* pBufferInfo);
    s32 cellAtracResetPlayPosition(void* handle, u32 uiSample, u32 uiWriteByte);
    s32 cellAtracGetInternalErrorInfo(void* handle, s32* piResult);

    s32 cellDmuxQueryAttr(const void* type, void* attr);
    s32 cellDmuxQueryEsAttr(const void* type, const void* esFilterId, const void* esSpecificInfo, void* esAttr);
    s32 cellDmuxOpen(const void* type, const void* res, const void* cb, void* handle);
    s32 cellDmuxClose(u32 handle);
    s32 cellDmuxEnableEs(u32 handle, const void* filterId, const void* res, const void* esCb, const void* esSpecificInfo, void* esHandle);
    s32 cellDmuxDisableEs(u32 esHandle);
    s32 cellDmuxReleaseAu(u32 esHandle);
    s32 cellDmuxSetStream(u32 handle, const void* streamAddr, u32 streamSize, u64 userData);
    s32 cellDmuxResetStream(u32 handle);
    s32 cellDmuxGetAu(u32 handle, u32 esHandle, void* auInfo);
    s32 cellDmuxGetAuEx(u32 handle, u32 esHandle, void* auInfoEx);
    s32 cellDmuxPeekAu(u32 handle, u32 esHandle, void* auInfo);
    s32 cellDmuxPeekAuEx(u32 handle, u32 esHandle, void* auInfoEx);
    s32 cellDmuxFlushEs(u32 handle, u32 esHandle);

    s32 cellVdecQueryAttr(const void* type, void* attr);
    s32 cellVdecQueryAttrEx(const void* type, void* attr);
    s32 cellVdecOpen(const void* type, const void* res, const void* cb, void* handle);
    s32 cellVdecOpenEx(const void* type, const void* res, const void* cb, void* handle);
    s32 cellVdecClose(u32 handle);
    s32 cellVdecStartSeq(u32 handle);
    s32 cellVdecEndSeq(u32 handle);
    s32 cellVdecDecodeAu(u32 handle, s32 mode, const void* auInfo);
    s32 cellVdecGetPicture(u32 handle, void* picItem);
    s32 cellVdecGetPicItem(u32 handle, void* picItem);
    s32 cellVdecSetFrameRate(u32 handle, u32 frameRateCode);

    s32 cellVpostQuery(const void* cfgParam, u32* memSize);
    s32 cellVpostQueryAttr(const void* cfgParam, u32* memSize);
    s32 cellVpostInit(const void* cfgParam, const void* resource, void* handle);
    s32 cellVpostOpen(const void* cfgParam, const void* resource, void* handle);
    s32 cellVpostEnd(u32 handle);
    s32 cellVpostClose(u32 handle);
    s32 cellVpostExec(u32 handle, const void* inPicBuf, const void* picInfo, void* outPicBuf, const void* ctrlParam);

    s32 cellAdecQueryAttr(const void* type, void* attr);
    s32 cellAdecOpen(const void* type, const void* res, const void* cb, void* handle);
    s32 cellAdecClose(u32 handle);
    s32 cellAdecStartSeq(u32 handle, void* param);
    s32 cellAdecEndSeq(u32 handle);
    s32 cellAdecDecodeAu(u32 handle, const void* auInfo);
    s32 cellAdecGetPcm(u32 handle, void* outBuffer);
    s32 cellAdecGetPcmItem(u32 handle, void* pcmItem);
}

// SPURS Fallback for fp=0x1AF3B10ECB1562C3 (Sony Edge LZMA Task)
static void edgelzma_fallback(spu_context* ctx) {
    (void)ctx;
    // Unblock the PPU waiting for this SPURS task if any
    cellSpursEventFlagSet((void*)(uintptr_t)0x01178880, 0x0001);
}

// Raw SPU fallback for fp=0x5009AA16A26A8923 (synth2 audio mixer)
static void synth2_fallback(spu_context* ctx) {
    (void)ctx;
    printf("[fallback] synth2 raw SPU workload registered\n");
}

extern "C" void ps3_load_prx_modules(void)
{
    // Register SPURS Workload fallbacks
    spu_workload_register(0x1AF3B10ECB1562C3ULL, edgelzma_fallback, "edgelzma");
    spu_workload_register(0x5009AA16A26A8923ULL, synth2_fallback, "synth2");

    // Critical NIDs registered here to guarantee resolution on any build
    ps3_hle_register(0x01220224u, "cellRescGcmSurface2RescSrc", (void*)cellRescGcmSurface2RescSrc);
    ps3_hle_register(0x0D3C22CEu, "cellRescSetWaitFlip", (void*)cellRescSetWaitFlip);
    ps3_hle_register(0xD1CA0503u, "cellRescVideoOutResolutionId2RescBufferMode", (void*)cellRescVideoOutResolutionId2RescBufferMode);
    ps3_hle_register(0x56DFE179u, "cellAudioSetPortLevel", (void*)cellAudioSetPortLevel);
    ps3_hle_register(0x45FE2FCEu, "_sys_spu_printf_initialize", (void*)_sys_spu_printf_initialize);
    ps3_hle_register(0x8A85674Du, "_cellSpursLFQueuePushBody", (void*)_cellSpursLFQueuePushBody);
    ps3_hle_register(0x73E06F91u, "cellSpursLFQueueDetachLv2EventQueue", (void*)cellSpursLFQueueDetachLv2EventQueue);

    ps3_hle_register(0xCA8181C1u, "cellPamfGetHeaderSize", (void*)cellPamfGetHeaderSize);
    ps3_hle_register(0x44F5C9E3u, "cellPamfGetStreamOffsetAndSize", (void*)cellPamfGetStreamOffsetAndSize);
    ps3_hle_register(0x28B4E2C1u, "cellPamfReaderSetStreamWithTypeAndIndex", (void*)cellPamfReaderSetStreamWithTypeAndIndex);
    ps3_hle_register(0x9AB20793u, "cellPamfReaderGetStreamTypeAndChannel", (void*)cellPamfReaderGetStreamTypeAndChannel);

    ps3_hle_register(0x0F9667B6u, "cellAtracGetChannel", (void*)cellAtracGetChannel);
    ps3_hle_register(0x2642D4CCu, "cellAtracCreateDecoderExt", (void*)cellAtracCreateDecoderExt);
    ps3_hle_register(0x2BFFF084u, "cellAtracGetStreamDataInfo", (void*)cellAtracGetStreamDataInfo);
    ps3_hle_register(0x46CFC013u, "cellAtracAddStreamData", (void*)cellAtracAddStreamData);
    ps3_hle_register(0x66AFC68Eu, "cellAtracSetDataAndGetMemSize", (void*)cellAtracSetDataAndGetMemSize);
    ps3_hle_register(0x761CB9BEu, "cellAtracDeleteDecoder", (void*)cellAtracDeleteDecoder);
    ps3_hle_register(0x78BA5C41u, "cellAtracSetLoopNum", (void*)cellAtracSetLoopNum);
    ps3_hle_register(0x8EB0E65Fu, "cellAtracDecode", (void*)cellAtracDecode);
    ps3_hle_register(0xAB6B6DBFu, "cellAtracGetLoopInfo", (void*)cellAtracGetLoopInfo);
    ps3_hle_register(0xB5C11938u, "cellAtracGetInternalErrorInfo", (void*)cellAtracGetInternalErrorInfo);
    ps3_hle_register(0xCF01D5D4u, "cellAtracGetSoundInfo", (void*)cellAtracGetSoundInfo);
    ps3_hle_register(0xDFAB73AAu, "cellAtracGetRemainFrame", (void*)cellAtracGetRemainFrame);

    // cellDmux
    ps3_hle_register(0xA2D4189Bu, "cellDmuxQueryAttr", (void*)cellDmuxQueryAttr);
    ps3_hle_register(0x02170D1Au, "cellDmuxQueryEsAttr", (void*)cellDmuxQueryEsAttr);
    ps3_hle_register(0x68492DE9u, "cellDmuxOpen", (void*)cellDmuxOpen);
    ps3_hle_register(0x8C692521u, "cellDmuxClose", (void*)cellDmuxClose);
    ps3_hle_register(0x7B56DC3Fu, "cellDmuxEnableEs", (void*)cellDmuxEnableEs);
    ps3_hle_register(0x05371C8Du, "cellDmuxDisableEs", (void*)cellDmuxDisableEs);
    ps3_hle_register(0x24EA6474u, "cellDmuxReleaseAu", (void*)cellDmuxReleaseAu);
    ps3_hle_register(0x04E7499Fu, "cellDmuxSetStream", (void*)cellDmuxSetStream);
    ps3_hle_register(0x5D345DE9u, "cellDmuxResetStream", (void*)cellDmuxResetStream);
    ps3_hle_register(0x42C716B5u, "cellDmuxGetAu", (void*)cellDmuxGetAu);
    ps3_hle_register(0x2C9A5857u, "cellDmuxGetAuEx", (void*)cellDmuxGetAuEx);
    ps3_hle_register(0x2750C5E0u, "cellDmuxPeekAu", (void*)cellDmuxPeekAu);
    ps3_hle_register(0x002E8DA2u, "cellDmuxPeekAuEx", (void*)cellDmuxPeekAuEx);
    ps3_hle_register(0xEBB3B2BDu, "cellDmuxFlushEs", (void*)cellDmuxFlushEs);

    // cellVdec
    ps3_hle_register(0xFF6F6EBEu, "cellVdecQueryAttr", (void*)cellVdecQueryAttr);
    ps3_hle_register(0xC982A84Au, "cellVdecQueryAttrEx", (void*)cellVdecQueryAttrEx);
    ps3_hle_register(0xB6BBCD5Du, "cellVdecOpen", (void*)cellVdecOpen);
    ps3_hle_register(0x0053E2D8u, "cellVdecOpenEx", (void*)cellVdecOpenEx);
    ps3_hle_register(0x16698E83u, "cellVdecClose", (void*)cellVdecClose);
    ps3_hle_register(0xC757C2AAu, "cellVdecStartSeq", (void*)cellVdecStartSeq);
    ps3_hle_register(0x824433F0u, "cellVdecEndSeq", (void*)cellVdecEndSeq);
    ps3_hle_register(0x2BF4DDD2u, "cellVdecDecodeAu", (void*)cellVdecDecodeAu);
    ps3_hle_register(0x807C861Au, "cellVdecGetPicture", (void*)cellVdecGetPicture);
    ps3_hle_register(0x17C702B9u, "cellVdecGetPicItem", (void*)cellVdecGetPicItem);
    ps3_hle_register(0xE13EF6FCu, "cellVdecSetFrameRate", (void*)cellVdecSetFrameRate);

    // cellVpost
    ps3_hle_register(0x49A099CDu, "cellVpostQuery", (void*)cellVpostQuery);
    ps3_hle_register(0x95E788C3u, "cellVpostQueryAttr", (void*)cellVpostQueryAttr);
    ps3_hle_register(0xC53308A9u, "cellVpostInit", (void*)cellVpostInit);
    ps3_hle_register(0xCD33F3E2u, "cellVpostOpen", (void*)cellVpostOpen);
    ps3_hle_register(0x0DCB4249u, "cellVpostEnd", (void*)cellVpostEnd);
    ps3_hle_register(0x10EF39F6u, "cellVpostClose", (void*)cellVpostClose);
    ps3_hle_register(0xABB8CC3Du, "cellVpostExec", (void*)cellVpostExec);

    // cellAdec
    ps3_hle_register(0x7E4A4A49u, "cellAdecQueryAttr", (void*)cellAdecQueryAttr);
    ps3_hle_register(0xD00A6988u, "cellAdecOpen", (void*)cellAdecOpen);
    ps3_hle_register(0x847D2380u, "cellAdecClose", (void*)cellAdecClose);
    ps3_hle_register(0x487B613Eu, "cellAdecStartSeq", (void*)cellAdecStartSeq);
    ps3_hle_register(0xE2EA549Bu, "cellAdecEndSeq", (void*)cellAdecEndSeq);
    ps3_hle_register(0x1529E506u, "cellAdecDecodeAu", (void*)cellAdecDecodeAu);
    ps3_hle_register(0x97FF2AF1u, "cellAdecGetPcm", (void*)cellAdecGetPcm);
    ps3_hle_register(0xBD75F78Bu, "cellAdecGetPcmItem", (void*)cellAdecGetPcmItem);
}
