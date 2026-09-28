/*
 * ps3recomp - cellVdec HLE implementation
 *
 * Stub video decoder. Accepts AU data and delivers AUDONE callbacks
 * but does not perform actual H.264/MPEG-2 decoding. Games that
 * require video playback will need an FFmpeg/libav integration here.
 */

#include "cellVdec.h"
#include <stdio.h>
#include <string.h>
#include "../guest_struct.h"   /* GUEST_EA, vm_read/vm_write: guest EA -> host */

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_VDEC 4

typedef struct {
    int in_use;
    u32 codecType;
    u32 cbFunc;     /* guest OPD */
    u32 cbArg;
    u32 resMemAddr;
    u32 resMemSize;
    int seqStarted;
    CellVdecPicItem lastPic;
    int hasPic;
    u32 auCount;        /* total AUs decoded */
    u16 width;          /* configured resolution (0 = use default) */
    u16 height;
} VdecSlot;

static VdecSlot s_vdec[MAX_VDEC];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellVdecQueryAttr(const CellVdecType* type, CellVdecAttr* attr)
{
    printf("[cellVdec] QueryAttr(codecType=%u)\n", type ? vm_read32(GUEST_EA(type)) : 0);

    if (!type || !attr)
        return (s32)CELL_VDEC_ERROR_ARG;

    u32 ea = GUEST_EA(attr);
    vm_write32(ea + (u32)offsetof(CellVdecAttr, memSize), 64 * 1024);
    vm_write32(ea + (u32)offsetof(CellVdecAttr, decoderVerUpper), 1);
    vm_write32(ea + (u32)offsetof(CellVdecAttr, decoderVerLower), 0);
    return CELL_OK;
}

s32 cellVdecQueryAttrEx(const CellVdecType* type, CellVdecAttr* attr)
{
    return cellVdecQueryAttr(type, attr);
}

s32 cellVdecOpen(const CellVdecType* type, const CellVdecResource* res,
                  const CellVdecCb* cb, CellVdecHandle* handle)
{
    u32 codec_type = type ? vm_read32(GUEST_EA(type)) : 0;
    printf("[cellVdec] Open(codecType=%u)\n", codec_type);

    if (!type || !handle)
        return (s32)CELL_VDEC_ERROR_ARG;

    u32 cbFunc = 0;
    u32 cbArg = 0;
    if (cb) {
        u32 cb_ea = GUEST_EA(cb);
        cbFunc = vm_read32(cb_ea + (u32)offsetof(CellVdecCb, cbFunc));
        cbArg  = vm_read32(cb_ea + (u32)offsetof(CellVdecCb, cbArg));
    }

    u32 resMemAddr = 0;
    u32 resMemSize = 0;
    if (res) {
        u32 res_ea = GUEST_EA(res);
        resMemAddr = vm_read32(res_ea + (u32)offsetof(CellVdecResource, memAddr));
        resMemSize = vm_read32(res_ea + (u32)offsetof(CellVdecResource, memSize));
    }

    for (int i = 0; i < MAX_VDEC; i++) {
        if (!s_vdec[i].in_use) {
            memset(&s_vdec[i], 0, sizeof(VdecSlot));
            s_vdec[i].in_use = 1;
            s_vdec[i].codecType = codec_type;
            s_vdec[i].cbFunc = cbFunc;
            s_vdec[i].cbArg = cbArg;
            s_vdec[i].resMemAddr = resMemAddr;
            s_vdec[i].resMemSize = resMemSize;
            vm_write32((u32)(uintptr_t)handle, (u32)i);
            printf("[cellVdec] Open -> handle=%u, cbFunc=0x%08X, cbArg=0x%08X, resMem=0x%08X\n",
                   i, cbFunc, cbArg, resMemAddr);
            return CELL_OK;
        }
    }
    return (s32)CELL_VDEC_ERROR_BUSY;
}

s32 cellVdecOpenEx(const CellVdecType* type, const CellVdecResource* res,
                    const CellVdecCb* cb, CellVdecHandle* handle)
{
    return cellVdecOpen(type, res, cb, handle);
}

s32 cellVdecClose(CellVdecHandle handle)
{
    printf("[cellVdec] Close(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    s_vdec[handle].in_use = 0;
    return CELL_OK;
}

s32 cellVdecStartSeq(CellVdecHandle handle)
{
    printf("[cellVdec] StartSeq(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    s_vdec[handle].seqStarted = 1;
    return CELL_OK;
}

s32 cellVdecEndSeq(CellVdecHandle handle)
{
    printf("[cellVdec] EndSeq(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    s_vdec[handle].seqStarted = 0;

    /* Notify sequence done */
    if (s_vdec[handle].cbFunc && g_ps3_guest_caller) {
        g_ps3_guest_caller(s_vdec[handle].cbFunc, (uint64_t)handle,
                           (uint64_t)CELL_VDEC_MSG_TYPE_SEQDONE,
                           (uint64_t)CELL_OK, (uint64_t)s_vdec[handle].cbArg,
                           0, 0, 0, 0);
    }

    return CELL_OK;
}

s32 cellVdecDecodeAu(CellVdecHandle handle, s32 mode, const CellVdecAuInfo* auInfo)
{
    (void)mode;

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;
    if (!auInfo)
        return (s32)CELL_VDEC_ERROR_ARG;

    VdecSlot* v = &s_vdec[handle];

    u32 au_ea = GUEST_EA(auInfo);
    CellVdecAuInfo au;
    au.startAddr = vm_read32(au_ea + (u32)offsetof(CellVdecAuInfo, startAddr));
    au.size      = vm_read32(au_ea + (u32)offsetof(CellVdecAuInfo, size));
    au.pts       = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, pts));
    au.dts       = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, dts));
    au.userData  = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, userData));

    printf("[cellVdec] DecodeAu(handle=%u, addr=0x%X, size=%u, pts=%llu)\n",
           handle, au.startAddr, au.size,
           (unsigned long long)au.pts);

    /* Step 1: Report AU consumed */
    if (v->cbFunc && g_ps3_guest_caller) {
        g_ps3_guest_caller(v->cbFunc, (uint64_t)handle,
                           (uint64_t)CELL_VDEC_MSG_TYPE_AUDONE,
                           (uint64_t)CELL_OK, (uint64_t)v->cbArg,
                           0, 0, 0, 0);
    }

    /* Step 2: Generate a dummy PICOUT callback */
    v->auCount++;
    memset(&v->lastPic, 0, sizeof(v->lastPic));
    v->lastPic.codecType = v->codecType;
    v->lastPic.startAddr = au.startAddr;
    v->lastPic.size      = au.size;
    v->lastPic.auNum     = v->auCount;
    v->lastPic.pts       = au.pts;
    v->lastPic.dts       = au.dts;
    v->lastPic.userData  = au.userData;
    v->lastPic.status    = 0; /* OK */
    v->lastPic.picFmt    = CELL_VDEC_PIC_FMT_YUV420P;
    v->lastPic.width     = v->width ? v->width : 1280;
    v->lastPic.height    = v->height ? v->height : 720;
    v->hasPic = 1;

    if (v->cbFunc && g_ps3_guest_caller) {
        g_ps3_guest_caller(v->cbFunc, (uint64_t)handle,
                           (uint64_t)CELL_VDEC_MSG_TYPE_PICOUT,
                           (uint64_t)CELL_OK, (uint64_t)v->cbArg,
                           0, 0, 0, 0);
    }

    return CELL_OK;
}

static void write_pic_item(VdecSlot* v, u32 item_ea)
{
    vm_write32(item_ea + 0x00, v->codecType);
    vm_write32(item_ea + 0x04, v->lastPic.startAddr);
    vm_write32(item_ea + 0x08, v->lastPic.size);
    vm_write32(item_ea + 0x0C, v->auCount ? v->auCount : 1);
    vm_write64(item_ea + 0x10, v->lastPic.pts);
    vm_write64(item_ea + 0x18, v->lastPic.pts);
    vm_write64(item_ea + 0x20, v->lastPic.userData);
    vm_write32(item_ea + 0x28, 0); /* OK */
    vm_write32(item_ea + 0x2C, CELL_VDEC_PIC_FMT_YUV420P);
    vm_write16(item_ea + 0x30, v->width ? v->width : 1280);
    vm_write16(item_ea + 0x32, v->height ? v->height : 720);
}

s32 cellVdecGetPicture(CellVdecHandle handle, void* picItem)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    VdecSlot* v = &s_vdec[handle];
    u32 item_ea = v->resMemAddr ? v->resMemAddr : 0x40000000;
    write_pic_item(v, item_ea);

    if (picItem) {
        vm_write32((u32)(uintptr_t)picItem, item_ea);
    }

    v->hasPic = 0;
    return CELL_OK;
}

s32 cellVdecGetPicItem(CellVdecHandle handle, void* picItem)
{
    return cellVdecGetPicture(handle, picItem);
}

s32 cellVdecSetFrameRate(CellVdecHandle handle, u32 frameRateCode)
{
    printf("[cellVdec] SetFrameRate(handle=%u, code=%u)\n",
           handle, frameRateCode);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    return CELL_OK;
}
