/*
 * ps3recomp - cellAtrac (libatrac3plus) HLE header
 *
 * Standalone ATRAC3+ audio decoder interface.
 */

#ifndef PS3RECOMP_CELL_ATRAC_H
#define PS3RECOMP_CELL_ATRAC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_ATRAC_ERROR_FACILITY_ATRAC              0x80610300
#define CELL_ATRAC_OK                                0x00000000
#define CELL_ATRAC_ERROR_API_FAIL                    0x80610301
#define CELL_ATRAC_ERROR_READSIZE_OVER_BUFFER        0x80610311
#define CELL_ATRAC_ERROR_UNKNOWN_FORMAT              0x80610312
#define CELL_ATRAC_ERROR_READSIZE_IS_TOO_SMALL       0x80610313
#define CELL_ATRAC_ERROR_ILLEGAL_SAMPLING_RATE       0x80610314
#define CELL_ATRAC_ERROR_ILLEGAL_DATA                0x80610315
#define CELL_ATRAC_ERROR_NO_DECODER                  0x80610321
#define CELL_ATRAC_ERROR_UNSET_DATA                  0x80610322
#define CELL_ATRAC_ERROR_DECODER_WAS_CREATED         0x80610323
#define CELL_ATRAC_ERROR_ALLDATA_WAS_DECODED         0x80610331
#define CELL_ATRAC_ERROR_NODATA_IN_BUFFER            0x80610332
#define CELL_ATRAC_ERROR_NOT_ALIGNED_OUT_BUFFER      0x80610333
#define CELL_ATRAC_ERROR_NEED_SECOND_BUFFER          0x80610334
#define CELL_ATRAC_ERROR_ALLDATA_IS_ONMEMORY         0x80610341
#define CELL_ATRAC_ERROR_ADD_DATA_IS_TOO_BIG         0x80610342
#define CELL_ATRAC_ERROR_NONEED_SECOND_BUFFER        0x80610351
#define CELL_ATRAC_ERROR_UNSET_LOOP_NUM              0x80610361
#define CELL_ATRAC_ERROR_ILLEGAL_SAMPLE              0x80610371
#define CELL_ATRAC_ERROR_ILLEGAL_RESET_BYTE          0x80610372
#define CELL_ATRAC_ERROR_ILLEGAL_PPU_THREAD_PRIORITY 0x80610381
#define CELL_ATRAC_ERROR_ILLEGAL_SPU_THREAD_PRIORITY 0x80610382

#define CELL_ATRAC_ALLDATA_IS_ON_MEMORY             (-1)
#define CELL_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY (-2)
#define CELL_ATRAC_LOOP_STREAM_DATA_IS_ON_MEMORY    (-3)

#define CELL_ATRAC_HANDLE_SIZE  512

typedef struct CellAtracHandle {
    u8 uiWorkMem[CELL_ATRAC_HANDLE_SIZE];
} CellAtracHandle;

typedef struct CellAtracBufferInfo {
    u32 pucWriteAddr;
    u32 uiWritableByte;
    u32 uiMinWriteByte;
    u32 uiReadPosition;
} CellAtracBufferInfo;

typedef struct CellAtracExtRes {
    u32 pSpurs;
    u8  priority[8];
} CellAtracExtRes;

/* Functions */
s32 cellAtracSetDataAndGetMemSize(CellAtracHandle* handle, void* pucBufferAddr,
                                  u32 uiReadByte, u32 uiBufferByte, u32* puiWorkMemSize);
s32 cellAtracCreateDecoder(CellAtracHandle* handle, void* pucWorkMem,
                           u32 uiPpuThreadPriority, u32 uiSpuThreadPriority);
s32 cellAtracCreateDecoderExt(CellAtracHandle* handle, void* pucWorkMem,
                              u32 uiWorkMemSize, CellAtracExtRes* pExtRes);
s32 cellAtracDeleteDecoder(CellAtracHandle* handle);
s32 cellAtracDecode(CellAtracHandle* handle, float* pOutPcm, u32* puiSamples,
                    u32* puiFinishFlag, s32* piRemainFrame);
s32 cellAtracAddStreamData(CellAtracHandle* handle, u32 uiAddByte);
s32 cellAtracGetSecondBufferInfo(CellAtracHandle* handle, u32* puiReadPosition, u32* puiWritableByte);
s32 cellAtracSetSecondBuffer(CellAtracHandle* handle, void* pucSecondBufferAddr, u32 uiSecondBufferByte);
s32 cellAtracGetRemainFrame(CellAtracHandle* handle, s32* piRemainFrame);
s32 cellAtracGetVacantSize(CellAtracHandle* handle, u32* puiVacantSize);
s32 cellAtracGetStreamDataInfo(CellAtracHandle* handle, void** ppucWriteAddr,
                               u32* puiWritableByte, u32* puiReadPosition);
s32 cellAtracGetChannel(CellAtracHandle* handle, u32* puiChannel);
s32 cellAtracGetMaxSample(CellAtracHandle* handle, u32* puiMaxSample);
s32 cellAtracGetNextSample(CellAtracHandle* handle, u32* puiNextSample);
s32 cellAtracGetSoundInfo(CellAtracHandle* handle, s32* piEndSample,
                          s32* piLoopStartSample, s32* piLoopEndSample);
s32 cellAtracGetNextDecodePosition(CellAtracHandle* handle, u32* puiNextDecodePosition);
s32 cellAtracGetBitrate(CellAtracHandle* handle, u32* puiBitrate);
s32 cellAtracGetLoopInfo(CellAtracHandle* handle, s32* piLoopNum, u32* puiLoopStatus);
s32 cellAtracIsSecondBufferNeeded(CellAtracHandle* handle);
s32 cellAtracSetLoopNum(CellAtracHandle* handle, s32 nLoopNum);
s32 cellAtracGetBufferInfoForResetting(CellAtracHandle* handle, u32 uiSample, CellAtracBufferInfo* pBufferInfo);
s32 cellAtracResetPlayPosition(CellAtracHandle* handle, u32 uiSample, u32 uiWriteByte);
s32 cellAtracGetInternalErrorInfo(CellAtracHandle* handle, s32* piResult);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_ATRAC_H */
