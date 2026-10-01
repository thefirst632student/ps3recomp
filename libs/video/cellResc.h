/*
 * ps3recomp - cellResc HLE
 *
 * Minimal RESC state/presentation support used by titles that render through
 * cellResc instead of registering/flipping GCM display buffers directly.
 */
#ifndef PS3RECOMP_CELL_RESC_H
#define PS3RECOMP_CELL_RESC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "cellGcmSys.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_RESC_ERROR_NOT_INITIALIZED    0x80210301u
#define CELL_RESC_ERROR_REINITIALIZED      0x80210302u
#define CELL_RESC_ERROR_BAD_ALIGNMENT      0x80210303u
#define CELL_RESC_ERROR_BAD_ARGUMENT       0x80210304u
#define CELL_RESC_ERROR_LESS_MEMORY        0x80210305u
#define CELL_RESC_ERROR_GCM_FLIP_QUE_FULL  0x80210306u
#define CELL_RESC_ERROR_BAD_COMBINATION    0x80210307u

#define CELL_RESC_720x480                  0x01u
#define CELL_RESC_720x576                  0x02u
#define CELL_RESC_1280x720                 0x04u
#define CELL_RESC_1920x1080                0x08u

typedef struct CellRescInitConfig {
    u32 size;
    u32 resourcePolicy;
    u32 supportModes;
    u32 ratioMode;
    u32 palTemporalMode;
    u32 interlaceMode;
    u32 flipMode;
} CellRescInitConfig;

/* SDK layout is 16 bytes: width/height are BE u16 fields. */
typedef struct CellRescSrc {
    u32 format;
    u32 pitch;
    u16 width;
    u16 height;
    u32 offset;
} CellRescSrc;

typedef struct CellRescDsts {
    u32 format;
    u32 pitch;
    u32 heightAlign;
} CellRescDsts;

typedef void (*CellRescFlipHandler)(u32 head);

s32 cellRescInit(const CellRescInitConfig* initConfig);
s32 cellRescVideoOutResolutionId2RescBufferMode(u32 resolutionId, u32* bufferMode);
s32 cellRescSetDsts(u32 bufferMode, const CellRescDsts* dsts);
s32 cellRescSetDisplayMode(u32 bufferMode);
s32 cellRescAdjustAspectRatio(float horizontal, float vertical);
s32 cellRescGetBufferSize(s32* colorBuffers, s32* vertexArray, s32* fragmentShader);
s32 cellRescGcmSurface2RescSrc(const void* gcmSurface, CellRescSrc* rescSrc);
s32 cellRescSetSrc(s32 idx, const CellRescSrc* src);
s32 cellRescSetConvertAndFlip(CellGcmContextData* context, s32 idx);
s32 cellRescSetWaitFlip(CellGcmContextData* context);
s32 cellRescSetBufferAddress(const u32* colorBuffers, const u32* vertexArray,
                             const u32* fragmentShader);
void cellRescSetFlipHandler(CellRescFlipHandler handler);

#ifdef __cplusplus
}
#endif
#endif
