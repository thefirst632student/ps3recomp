/*
 * ps3recomp - cellVdec HLE
 *
 * Video decoder: H.264/AVC and MPEG-2 video decoding.
 * Receives AU (access units) from cellDmux and outputs decoded frames.
 */

#ifndef PS3RECOMP_CELL_VDEC_H
#define PS3RECOMP_CELL_VDEC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_VDEC_ERROR_ARG            0x80610101
#define CELL_VDEC_ERROR_SEQ            0x80610102
#define CELL_VDEC_ERROR_BUSY           0x80610103
#define CELL_VDEC_ERROR_EMPTY          0x80610104
#define CELL_VDEC_ERROR_AU             0x80610105
#define CELL_VDEC_ERROR_PIC            0x80610106
#define CELL_VDEC_ERROR_FATAL          0x80610180

/* ---------------------------------------------------------------------------
 * Codec types
 * -----------------------------------------------------------------------*/
#define CELL_VDEC_CODEC_TYPE_MPEG2     0x00000000
#define CELL_VDEC_CODEC_TYPE_AVC       0x00000001
#define CELL_VDEC_CODEC_TYPE_MPEG4     0x00000002
#define CELL_VDEC_CODEC_TYPE_DIVX      0x00000005

/* Picture format */
#define CELL_VDEC_PICFMT_ARGB32_ILV    0
#define CELL_VDEC_PICFMT_RGBA32_ILV    1
#define CELL_VDEC_PICFMT_UYVY422_ILV   2
#define CELL_VDEC_PICFMT_YUV420_PLANAR 3

/* Callback message types */
#define CELL_VDEC_MSG_TYPE_AUDONE      0
#define CELL_VDEC_MSG_TYPE_PICOUT      1
#define CELL_VDEC_MSG_TYPE_SEQDONE     2
#define CELL_VDEC_MSG_TYPE_ERROR       3

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 CellVdecHandle;

typedef struct CellVdecType {
    u32 codecType;
    u32 profileLevel;
} CellVdecType;

typedef struct CellVdecResource {
    u32 memAddr;
    u32 memSize;
    u32 ppuThreadPrio;
    u32 ppuThreadStackSize;
    u32 spuThreadPrio;
    u32 numOfSpus;
} CellVdecResource;

typedef struct CellVdecAuInfo {
    u32 startAddr;
    u32 size;
    u64 pts;
    u64 dts;
    u64 userData;
} CellVdecAuInfo;

typedef struct CellVdecPicFormat {
    u32 formatType;
    u32 colorMatrixType;
    u8 alpha;
} CellVdecPicFormat;

/* Guest ABI is written by explicit offsets in cellVdec.c. */
typedef struct CellVdecPicItem { u8 opaque[0x4c]; } CellVdecPicItem;

typedef struct CellVdecCb {
    u32 cbFunc;
    u32 cbArg;
} CellVdecCb;

typedef struct CellVdecAttr {
    u32 memSize;
    u8 cmdDepth;
    u32 decoderVerUpper;
    u32 decoderVerLower;
} CellVdecAttr;

typedef u32 (*CellVdecCbMsg)(CellVdecHandle handle, u32 msgType,
                               s32 msgData, void* cbArg);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellVdecQueryAttr(const CellVdecType* type, CellVdecAttr* attr);
s32 cellVdecQueryAttrEx(const CellVdecType* type, CellVdecAttr* attr);

s32 cellVdecOpen(const CellVdecType* type, const CellVdecResource* res,
                  const CellVdecCb* cb, CellVdecHandle* handle);
s32 cellVdecOpenEx(const CellVdecType* type, const CellVdecResource* res,
                    const CellVdecCb* cb, CellVdecHandle* handle);
s32 cellVdecClose(CellVdecHandle handle);

s32 cellVdecStartSeq(CellVdecHandle handle);
s32 cellVdecEndSeq(CellVdecHandle handle);

s32 cellVdecDecodeAu(CellVdecHandle handle, s32 mode, const CellVdecAuInfo* auInfo);

s32 cellVdecGetPicture(CellVdecHandle handle, const CellVdecPicFormat* format, void* outBuff);
s32 cellVdecGetPicItem(CellVdecHandle handle, void* picItem);

s32 cellVdecSetFrameRate(CellVdecHandle handle, u32 frameRateCode);

int cellVdec_is_seq_active(void);

/* Host lifecycle hint used by the title-specific movie worker bridge.  Once
 * vpostStart exits there is no picture consumer left, so a decoder blocked at
 * the output high-water mark must switch to teardown drop mode immediately. */
void cellVdec_notify_output_consumer_stopped(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VDEC_H */
