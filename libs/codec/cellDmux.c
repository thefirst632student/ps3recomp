/*
 * ps3recomp - cellDmux HLE implementation
 *
 * Stub demuxer that accepts PAMF data and reports AU-found callbacks.
 * Without real MPEG-TS parsing, this provides the API surface games
 * expect while the video pipeline is built out.
 */

#include "cellDmux.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    int in_use;
    u32 cbFunc;
    u32 cbArg;
    u32 resMemAddr;
    u32 resMemSize;
    u32 streamType;
    /* Stream data tracking for demux processing */
    u32 streamAddr;
    u32 streamSize;
    u64 userData;
    u32 auSeqNo;        /* running AU sequence counter */
} DmuxSlot;

typedef struct {
    int in_use;
    u32 dmuxId;
    CellDmuxEsFilterId filterId;
    u32 esCbFunc;
    u32 esCbArg;
    u32 memAddr;
    u32 memSize;
    CellDmuxAuInfo currentAu;
    int hasAu;
} DmuxEsSlot;

static DmuxSlot s_dmux[CELL_DMUX_MAX_HANDLES];
static DmuxEsSlot s_es[CELL_DMUX_MAX_ES];

/* ---------------------------------------------------------------------------
 * Query attributes
 * -----------------------------------------------------------------------*/

s32 cellDmuxQueryAttr(const CellDmuxType* type, CellDmuxAttr* attr)
{
    printf("[cellDmux] QueryAttr(type=%u)\n", type ? vm_read32(GUEST_EA(type)) : 0);

    if (!type || !attr)
        return (s32)CELL_DMUX_ERROR_ARG;

    u32 ea = GUEST_EA(attr);
    vm_write32(ea + (u32)offsetof(CellDmuxAttr, memSize), 64 * 1024);
    vm_write32(ea + (u32)offsetof(CellDmuxAttr, demuxerVerUpper), 1);
    vm_write32(ea + (u32)offsetof(CellDmuxAttr, demuxerVerLower), 0);
    return CELL_OK;
}

s32 cellDmuxQueryEsAttr(const CellDmuxType* type, const CellDmuxEsFilterId* esFilterId,
                         const void* esSpecificInfo, CellDmuxEsAttr* esAttr)
{
    (void)esSpecificInfo;
    (void)esFilterId;

    printf("[cellDmux] QueryEsAttr()\n");

    if (!type || !esAttr)
        return (s32)CELL_DMUX_ERROR_ARG;

    u32 ea = GUEST_EA(esAttr);
    vm_write32(ea + (u32)offsetof(CellDmuxEsAttr, memSize), 64 * 1024);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Demuxer lifecycle
 * -----------------------------------------------------------------------*/

s32 cellDmuxOpen(const CellDmuxType* type, const CellDmuxResource* res,
                  const CellDmuxCb* cb, CellDmuxHandle* handle)
{
    printf("[cellDmux] Open(streamType=%u)\n",
           type ? vm_read32(GUEST_EA(type)) : 0);

    if (!type || !handle)
        return (s32)CELL_DMUX_ERROR_ARG;

    u32 cbFunc = 0;
    u32 cbArg = 0;
    if (cb) {
        u32 cb_ea = GUEST_EA(cb);
        cbFunc = vm_read32(cb_ea + (u32)offsetof(CellDmuxCb, cbFunc));
        cbArg  = vm_read32(cb_ea + (u32)offsetof(CellDmuxCb, cbArg));
    }

    u32 resMemAddr = 0;
    u32 resMemSize = 0;
    if (res) {
        u32 res_ea = GUEST_EA(res);
        resMemAddr = vm_read32(res_ea + (u32)offsetof(CellDmuxResource, memAddr));
        resMemSize = vm_read32(res_ea + (u32)offsetof(CellDmuxResource, memSize));
    }

    for (int i = 0; i < CELL_DMUX_MAX_HANDLES; i++) {
        if (!s_dmux[i].in_use) {
            s_dmux[i].in_use = 1;
            s_dmux[i].cbFunc = cbFunc;
            s_dmux[i].cbArg = cbArg;
            s_dmux[i].resMemAddr = resMemAddr;
            s_dmux[i].resMemSize = resMemSize;
            s_dmux[i].streamType = vm_read32(GUEST_EA(type));
            s_dmux[i].streamAddr = 0;
            s_dmux[i].streamSize = 0;
            s_dmux[i].userData = 0;
            s_dmux[i].auSeqNo = 0;
            vm_write32((u32)(uintptr_t)handle, (u32)i);
            printf("[cellDmux] Open -> handle=%u, cbFunc=0x%08X, cbArg=0x%08X\n", i, cbFunc, cbArg);
            return CELL_OK;
        }
    }
    return (s32)CELL_DMUX_ERROR_BUSY;
}

s32 cellDmuxClose(CellDmuxHandle handle)
{
    printf("[cellDmux] Close(handle=%u)\n", handle);

    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    /* Close associated ES handles */
    for (int i = 0; i < CELL_DMUX_MAX_ES; i++) {
        if (s_es[i].in_use && s_es[i].dmuxId == handle)
            s_es[i].in_use = 0;
    }

    s_dmux[handle].in_use = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * ES management
 * -----------------------------------------------------------------------*/

s32 cellDmuxEnableEs(CellDmuxHandle handle, const CellDmuxEsFilterId* esFilterId,
                      const CellDmuxEsResource* esRes,
                      const CellDmuxEsCb* esCb, const void* esSpecificInfo,
                      CellDmuxEsHandle* esHandle)
{
    (void)esSpecificInfo;

    printf("[cellDmux] EnableEs(dmux=%u)\n", handle);

    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;
    if (!esFilterId || !esHandle)
        return (s32)CELL_DMUX_ERROR_ARG;

    u32 esCbFunc = 0;
    u32 esCbArg = 0;
    if (esCb) {
        u32 esCb_ea = GUEST_EA(esCb);
        esCbFunc = vm_read32(esCb_ea + (u32)offsetof(CellDmuxEsCb, cbFunc));
        esCbArg  = vm_read32(esCb_ea + (u32)offsetof(CellDmuxEsCb, cbArg));
    }

    u32 memAddr = 0;
    u32 memSize = 0;
    if (esRes) {
        u32 esRes_ea = GUEST_EA(esRes);
        memAddr = vm_read32(esRes_ea + (u32)offsetof(CellDmuxEsResource, memAddr));
        memSize = vm_read32(esRes_ea + (u32)offsetof(CellDmuxEsResource, memSize));
    }

    for (int i = 0; i < CELL_DMUX_MAX_ES; i++) {
        if (!s_es[i].in_use) {
            s_es[i].in_use = 1;
            s_es[i].dmuxId = handle;
            guest_struct_load(&s_es[i].filterId, GUEST_EA(esFilterId),
                              (u32)sizeof(s_es[i].filterId));
            s_es[i].esCbFunc = esCbFunc;
            s_es[i].esCbArg = esCbArg;
            s_es[i].memAddr = memAddr;
            s_es[i].memSize = memSize;
            s_es[i].hasAu = 0;
            memset(&s_es[i].currentAu, 0, sizeof(CellDmuxAuInfo));
            vm_write32((u32)(uintptr_t)esHandle, (u32)i);
            printf("[cellDmux] EnableEs -> esHandle=%u, esCbFunc=0x%08X, esCbArg=0x%08X, memAddr=0x%08X\n",
                   i, esCbFunc, esCbArg, memAddr);
            return CELL_OK;
        }
    }
    return (s32)CELL_DMUX_ERROR_BUSY;
}

s32 cellDmuxDisableEs(CellDmuxEsHandle esHandle)
{
    printf("[cellDmux] DisableEs(es=%u)\n", esHandle);

    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    s_es[esHandle].in_use = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Data feeding
 * -----------------------------------------------------------------------*/

s32 cellDmuxSetStream(CellDmuxHandle handle, u32 streamAddr, u32 streamSize,
                       b8 discontinuity, u64 userData)
{
    printf("[cellDmux] SetStream(handle=%u, addr=0x%X, size=%u, discont=%d)\n",
           handle, streamAddr, streamSize, discontinuity);

    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    DmuxSlot* dmux = &s_dmux[handle];

    /* Track the stream data */
    dmux->streamAddr = streamAddr;
    dmux->streamSize = streamSize;
    dmux->userData = userData;

    if (discontinuity) {
        /* On discontinuity, reset AU sequence and clear pending AUs */
        dmux->auSeqNo = 0;
        for (int i = 0; i < CELL_DMUX_MAX_ES; i++) {
            if (s_es[i].in_use && s_es[i].dmuxId == handle)
                s_es[i].hasAu = 0;
        }
    }

    /*
     * Synthesize one AU per enabled ES from the entire stream buffer.
     * Real demux would parse PES/MPEG-TS headers; here we present the
     * whole input as a single Access Unit per elementary stream, which
     * is sufficient for games that simply shuttle data through the
     * demux -> vdec/adec pipeline.
     */
    for (int i = 0; i < CELL_DMUX_MAX_ES; i++) {
        if (!s_es[i].in_use || s_es[i].dmuxId != handle)
            continue;

        DmuxEsSlot* es = &s_es[i];

        /* Fill in AU info */
        es->currentAu.auAddr = streamAddr;
        es->currentAu.auSize = streamSize;
        es->currentAu.pts    = (u64)dmux->auSeqNo * 3003; /* ~29.97 fps tick */
        es->currentAu.dts    = es->currentAu.pts;
        es->currentAu.userData = userData;
        es->currentAu.isRap  = (dmux->auSeqNo == 0) ? 1 : 0;
        es->currentAu.reserved = 0;
        es->hasAu = 1;

        printf("[cellDmux] ES %d: AU_FOUND (addr=0x%X, size=%u, pts=%llu)\n",
               i, streamAddr, streamSize, (unsigned long long)es->currentAu.pts);

        /* Fire AU_FOUND callback to guest */
        if (es->esCbFunc && g_ps3_guest_caller) {
            u32 es_msg_ea = es->memAddr ? (es->memAddr + 0x40) : 0;
            if (es_msg_ea) {
                vm_write32(es_msg_ea + 0x00, CELL_DMUX_ES_MSG_TYPE_AU_FOUND);
                vm_write32(es_msg_ea + 0x04, 0);
                vm_write64(es_msg_ea + 0x08, dmux->userData);
            }
            g_ps3_guest_caller(es->esCbFunc, (uint64_t)handle, (uint64_t)i,
                               (uint64_t)es_msg_ea, (uint64_t)es->esCbArg,
                               0, 0, 0, 0);
        }
    }

    dmux->auSeqNo++;

    /* Signal demux completion after all ES callbacks have fired */
    if (dmux->cbFunc && g_ps3_guest_caller) {
        u32 msg_ea = dmux->resMemAddr ? (dmux->resMemAddr + 0x00) : 0;
        if (msg_ea) {
            vm_write32(msg_ea + 0x00, CELL_DMUX_MSG_TYPE_DEMUX_DONE);
            vm_write32(msg_ea + 0x04, 0);
            vm_write64(msg_ea + 0x08, dmux->userData);
        }
        g_ps3_guest_caller(dmux->cbFunc, (uint64_t)handle, (uint64_t)msg_ea,
                           (uint64_t)dmux->cbArg, 0, 0, 0, 0, 0);
    }

    return CELL_OK;
}

s32 cellDmuxResetStream(CellDmuxHandle handle)
{
    printf("[cellDmux] ResetStream(handle=%u)\n", handle);

    if (handle >= CELL_DMUX_MAX_HANDLES || !s_dmux[handle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    /* Clear pending AUs from all ES handles and reset stream tracking */
    for (int i = 0; i < CELL_DMUX_MAX_ES; i++) {
        if (s_es[i].in_use && s_es[i].dmuxId == handle)
            s_es[i].hasAu = 0;
    }
    s_dmux[handle].streamAddr = 0;
    s_dmux[handle].streamSize = 0;
    s_dmux[handle].auSeqNo = 0;

    return CELL_OK;
}

s32 cellDmuxResetStreamAndWaitDone(CellDmuxHandle handle)
{
    return cellDmuxResetStream(handle);
}

/* ---------------------------------------------------------------------------
 * AU retrieval
 * -----------------------------------------------------------------------*/

static s32 write_au_info(CellDmuxEsHandle esHandle, void* auInfo, u32* auInfoNum, int is_ex)
{
    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    if (!s_es[esHandle].hasAu)
        return (s32)CELL_DMUX_ERROR_EMPTY;

    DmuxEsSlot* es = &s_es[esHandle];
    u32 guest_au_addr = es->memAddr;
    if (guest_au_addr) {
        vm_write32(guest_au_addr + 0x00, es->currentAu.auAddr);
        vm_write32(guest_au_addr + 0x04, es->currentAu.auSize);
        vm_write32(guest_au_addr + 0x08, 0);
        vm_write32(guest_au_addr + 0x0C, 0);
        vm_write64(guest_au_addr + 0x10, es->currentAu.dts);
        vm_write64(guest_au_addr + 0x18, es->currentAu.pts);
        vm_write64(guest_au_addr + 0x20, es->currentAu.userData);
        vm_write32(guest_au_addr + 0x28, es->currentAu.isRap);
        vm_write32(guest_au_addr + 0x2C, 0);
        if (is_ex) {
            vm_write32(guest_au_addr + 0x30, 0);
        }
        if (auInfo) {
            vm_write32((u32)(uintptr_t)auInfo, guest_au_addr);
        }
    }

    if (auInfoNum)
        vm_write32((u32)(uintptr_t)auInfoNum, (u32)1);

    return CELL_OK;
}

s32 cellDmuxGetAu(CellDmuxEsHandle esHandle, void* auInfo, u32* auInfoNum)
{
    return write_au_info(esHandle, auInfo, auInfoNum, 0);
}

s32 cellDmuxGetAuEx(CellDmuxEsHandle esHandle, void* auInfoEx, u32* auInfoNum)
{
    return write_au_info(esHandle, auInfoEx, auInfoNum, 1);
}

s32 cellDmuxPeekAu(CellDmuxEsHandle esHandle, void* auInfo, u32* auInfoNum)
{
    return write_au_info(esHandle, auInfo, auInfoNum, 0);
}

s32 cellDmuxPeekAuEx(CellDmuxEsHandle esHandle, void* auInfoEx, u32* auInfoNum)
{
    return write_au_info(esHandle, auInfoEx, auInfoNum, 1);
}

s32 cellDmuxReleaseAu(CellDmuxEsHandle esHandle)
{
    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    s_es[esHandle].hasAu = 0;
    return CELL_OK;
}

s32 cellDmuxFlushEs(CellDmuxEsHandle esHandle)
{
    printf("[cellDmux] FlushEs(es=%u)\n", esHandle);

    if (esHandle >= CELL_DMUX_MAX_ES || !s_es[esHandle].in_use)
        return (s32)CELL_DMUX_ERROR_ARG;

    s_es[esHandle].hasAu = 0;

    if (s_es[esHandle].esCbFunc && g_ps3_guest_caller) {
        u32 es_msg_ea = s_es[esHandle].memAddr ? (s_es[esHandle].memAddr + 0x40) : 0;
        if (es_msg_ea) {
            vm_write32(es_msg_ea + 0x00, CELL_DMUX_ES_MSG_TYPE_FLUSH_DONE);
            vm_write32(es_msg_ea + 0x04, 0);
            vm_write64(es_msg_ea + 0x08, 0);
        }
        g_ps3_guest_caller(s_es[esHandle].esCbFunc, (uint64_t)s_es[esHandle].dmuxId,
                           (uint64_t)esHandle, (uint64_t)es_msg_ea,
                           (uint64_t)s_es[esHandle].esCbArg, 0, 0, 0, 0);
    }

    return CELL_OK;
}
