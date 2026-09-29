/*
 * ps3recomp - cellVpost HLE
 *
 * PS3 cellVpost ABI subset used by titles for YUV420 video post-processing.
 * Layouts and enum values follow the PS3 ABI (and RPCS3's public module
 * definitions), not the earlier placeholder structs that were in this port.
 */

#ifndef PS3RECOMP_CELL_VPOST_H
#define PS3RECOMP_CELL_VPOST_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* cellVpost errors used by this implementation. */
#define CELL_VPOST_ERROR_Q_ARG_CFG_NULL          ((u32)0x80610410u)
#define CELL_VPOST_ERROR_Q_ARG_CFG_INVALID       ((u32)0x80610411u)
#define CELL_VPOST_ERROR_Q_ARG_ATTR_NULL         ((u32)0x80610412u)
#define CELL_VPOST_ERROR_O_ARG_CFG_NULL          ((u32)0x80610440u)
#define CELL_VPOST_ERROR_O_ARG_CFG_INVALID       ((u32)0x80610441u)
#define CELL_VPOST_ERROR_O_ARG_RSRC_NULL         ((u32)0x80610442u)
#define CELL_VPOST_ERROR_O_ARG_RSRC_INVALID      ((u32)0x80610443u)
#define CELL_VPOST_ERROR_O_ARG_HDL_NULL          ((u32)0x80610444u)
#define CELL_VPOST_ERROR_C_ARG_HDL_INVALID       ((u32)0x80610471u)
#define CELL_VPOST_ERROR_E_ARG_HDL_INVALID       ((u32)0x806104A1u)
#define CELL_VPOST_ERROR_E_ARG_INPICBUF_NULL     ((u32)0x806104A2u)
#define CELL_VPOST_ERROR_E_ARG_CTRL_NULL         ((u32)0x806104A4u)
#define CELL_VPOST_ERROR_E_ARG_CTRL_INVALID      ((u32)0x806104A5u)
#define CELL_VPOST_ERROR_E_ARG_OUTPICBUF_NULL    ((u32)0x806104A6u)
#define CELL_VPOST_ERROR_E_ARG_PICINFO_NULL      ((u32)0x806104A8u)

#define CELL_VPOST_HANDLE_MAX 4

typedef u32 CellVpostHandle;

enum {
    CELL_VPOST_PIC_DEPTH_8 = 0
};

enum {
    CELL_VPOST_PIC_FMT_IN_YUV420_PLANAR = 0
};

enum {
    CELL_VPOST_PIC_FMT_OUT_RGBA_ILV = 0,
    CELL_VPOST_PIC_FMT_OUT_YUV420_PLANAR = 1
};

enum {
    CELL_VPOST_PIC_STRUCT_PFRM = 0,
    CELL_VPOST_PIC_STRUCT_IFRM = 1,
    CELL_VPOST_PIC_STRUCT_ITOP = 2,
    CELL_VPOST_PIC_STRUCT_IBTM = 3
};

enum {
    CELL_VPOST_SCAN_TYPE_P = 0,
    CELL_VPOST_SCAN_TYPE_I = 1
};

enum {
    CELL_VPOST_QUANT_RANGE_FULL = 0,
    CELL_VPOST_QUANT_RANGE_BROADCAST = 1
};

enum {
    CELL_VPOST_COLOR_MATRIX_BT601 = 0,
    CELL_VPOST_COLOR_MATRIX_BT709 = 1
};

typedef struct CellVpostCfgParam {
    u32 inMaxWidth;
    u32 inMaxHeight;
    s32 inDepth;
    s32 inPicFmt;
    u32 outMaxWidth;
    u32 outMaxHeight;
    s32 outDepth;
    s32 outPicFmt;
    u32 reserved1;
    u32 reserved2;
} CellVpostCfgParam;

typedef struct CellVpostAttr {
    u32 memSize;
    u8  delay;
    u8  _pad[3];
    u32 vpostVerUpper;
    u32 vpostVerLower;
} CellVpostAttr;

typedef struct CellVpostResource {
    u32 memAddr;
    u32 memSize;
    s32 ppuThreadPriority;
    u32 ppuThreadStackSize;
    s32 spuThreadPriority;
    u32 numOfSpus;
} CellVpostResource;

typedef struct CellVpostWindow {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
} CellVpostWindow;

/* Guest ABI size: 0x60. userData is at +0x50. */
typedef struct CellVpostCtrlParam {
    s32 execType;
    s32 scalerType;
    s32 ipcType;
    u32 inWidth;
    u32 inHeight;
    s32 inChromaPosType;
    s32 inQuantRange;
    s32 inColorMatrix;
    CellVpostWindow inWindow;
    u32 outWidth;
    u32 outHeight;
    CellVpostWindow outWindow;
    u8  outAlpha;
    u8  _pad0[7];
    u64 userData;
    u32 reserved1;
    u32 reserved2;
} CellVpostCtrlParam;

/* Guest ABI size: 0x58. */
typedef struct CellVpostPictureInfo {
    u32 inWidth;
    u32 inHeight;
    s32 inDepth;
    s32 inScanType;
    s32 inPicFmt;
    s32 inChromaPosType;
    s32 inPicStruct;
    s32 inQuantRange;
    s32 inColorMatrix;
    u32 outWidth;
    u32 outHeight;
    s32 outDepth;
    s32 outScanType;
    s32 outPicFmt;
    s32 outChromaPosType;
    s32 outPicStruct;
    s32 outQuantRange;
    s32 outColorMatrix;
    u64 userData;
    u32 reserved1;
    u32 reserved2;
} CellVpostPictureInfo;

/* Legacy helper retained because the generated registration table exports it. */
s32 cellVpostQuery(const CellVpostCfgParam* cfgParam, u32* memSize);
s32 cellVpostQueryAttr(const CellVpostCfgParam* cfgParam, CellVpostAttr* attr);
s32 cellVpostInit(const CellVpostCfgParam* cfgParam,
                  const CellVpostResource* resource,
                  CellVpostHandle* handle);
s32 cellVpostOpen(const CellVpostCfgParam* cfgParam,
                  const CellVpostResource* resource,
                  CellVpostHandle* handle);
s32 cellVpostEnd(CellVpostHandle handle);
s32 cellVpostClose(CellVpostHandle handle);

/* Real PS3 ABI order: handle, inPicBuff, ctrlParam, outPicBuff, picInfo. */
s32 cellVpostExec(CellVpostHandle handle,
                  const void* inPicBuf,
                  const CellVpostCtrlParam* ctrlParam,
                  void* outPicBuf,
                  CellVpostPictureInfo* picInfo);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VPOST_H */
