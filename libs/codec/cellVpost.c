/*
 * ps3recomp - cellVpost HLE
 *
 * Implements the ABI/layout actually used by Cell SDK cellVpost.  WA2 feeds
 * planar YUV420 pictures produced by cellVdec and requests RGBA.  Besides
 * writing the guest output buffer (so the original vdisp path remains valid),
 * the converted RGBA frame is presented through the host D3D12 movie path.
 * This bypasses gaps in the guest RSX movie blit path without bypassing PAMF,
 * demux, AVC decode, timestamps, or the game's media worker threads.
 */
#include "cellVpost.h"
#include "../guest_struct.h"
#include "../video/rsx_d3d12_backend.h"
#include "../../runtime/memory/vm.h"
#include "../../runtime/ppu/ppu_memory.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef struct VpostHandleState {
    int in_use;
    int movie_mode;
    CellVpostCfgParam cfg;
    u64 exec_count;
} VpostHandleState;

static VpostHandleState s_handles[CELL_VPOST_HANDLE_MAX];
static int s_movie_users;

static inline u8 clamp8(int v)
{
    return (u8)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void load_cfg(CellVpostCfgParam* c, u32 ea)
{
    memset(c, 0, sizeof(*c));
    if (!ea) return;
    c->inMaxWidth   = vm_read32(ea + 0x00);
    c->inMaxHeight  = vm_read32(ea + 0x04);
    c->inDepth      = (s32)vm_read32(ea + 0x08);
    c->inPicFmt     = (s32)vm_read32(ea + 0x0c);
    c->outMaxWidth  = vm_read32(ea + 0x10);
    c->outMaxHeight = vm_read32(ea + 0x14);
    c->outDepth     = (s32)vm_read32(ea + 0x18);
    c->outPicFmt    = (s32)vm_read32(ea + 0x1c);
    c->reserved1    = vm_read32(ea + 0x20);
    c->reserved2    = vm_read32(ea + 0x24);
}

static void store_pic_info(u32 ea, u32 iw, u32 ih, u32 ow, u32 oh,
                           s32 chroma, s32 quant, s32 matrix, u64 user)
{
    if (!ea) return;
    vm_write32(ea + 0x00, iw);
    vm_write32(ea + 0x04, ih);
    vm_write32(ea + 0x08, CELL_VPOST_PIC_DEPTH_8);
    vm_write32(ea + 0x0c, CELL_VPOST_SCAN_TYPE_P);
    vm_write32(ea + 0x10, CELL_VPOST_PIC_FMT_IN_YUV420_PLANAR);
    vm_write32(ea + 0x14, (u32)chroma);
    vm_write32(ea + 0x18, CELL_VPOST_PIC_STRUCT_PFRM);
    vm_write32(ea + 0x1c, (u32)quant);
    vm_write32(ea + 0x20, (u32)matrix);
    vm_write32(ea + 0x24, ow);
    vm_write32(ea + 0x28, oh);
    vm_write32(ea + 0x2c, CELL_VPOST_PIC_DEPTH_8);
    vm_write32(ea + 0x30, CELL_VPOST_SCAN_TYPE_P);
    vm_write32(ea + 0x34, CELL_VPOST_PIC_FMT_OUT_RGBA_ILV);
    vm_write32(ea + 0x38, (u32)chroma);
    vm_write32(ea + 0x3c, CELL_VPOST_PIC_STRUCT_PFRM);
    vm_write32(ea + 0x40, (u32)quant);
    vm_write32(ea + 0x44, (u32)matrix);
    vm_write64(ea + 0x48, user);
    vm_write32(ea + 0x50, 0);
    vm_write32(ea + 0x54, 0);
}

static void yuv_to_rgba(const u8* yplane, const u8* uplane, const u8* vplane,
                        u8* out, u32 iw, u32 ih, u32 ow, u32 oh,
                        s32 quant, s32 matrix, u8 alpha)
{
    /* Nearest-neighbour scaling is sufficient for the WA2 path (1280x720 ->
     * 1280x720) and keeps this HLE independent of a host FFmpeg ABI. */
    for (u32 dy = 0; dy < oh; ++dy) {
        const u32 sy = (u32)(((u64)dy * ih) / oh);
        const u32 cy = sy >> 1;
        u8* dst = out + (size_t)dy * ow * 4u;
        for (u32 dx = 0; dx < ow; ++dx) {
            const u32 sx = (u32)(((u64)dx * iw) / ow);
            const int Y = yplane[(size_t)sy * iw + sx];
            const int U = uplane[(size_t)cy * (iw >> 1) + (sx >> 1)];
            const int V = vplane[(size_t)cy * (iw >> 1) + (sx >> 1)];
            const int D = U - 128;
            const int E = V - 128;
            int r, g, b;
            if (quant == CELL_VPOST_QUANT_RANGE_BROADCAST) {
                int C = Y - 16;
                if (C < 0) C = 0;
                if (matrix == CELL_VPOST_COLOR_MATRIX_BT709) {
                    r = (298 * C + 459 * E + 128) >> 8;
                    g = (298 * C -  55 * D - 136 * E + 128) >> 8;
                    b = (298 * C + 541 * D + 128) >> 8;
                } else {
                    r = (298 * C + 409 * E + 128) >> 8;
                    g = (298 * C - 100 * D - 208 * E + 128) >> 8;
                    b = (298 * C + 516 * D + 128) >> 8;
                }
            } else {
                if (matrix == CELL_VPOST_COLOR_MATRIX_BT709) {
                    r = Y + ((403 * E) >> 8);
                    g = Y - (( 48 * D + 120 * E) >> 8);
                    b = Y + ((475 * D) >> 8);
                } else {
                    r = Y + ((359 * E) >> 8);
                    g = Y - (( 88 * D + 183 * E) >> 8);
                    b = Y + ((454 * D) >> 8);
                }
            }
            dst[dx * 4u + 0] = clamp8(r);
            dst[dx * 4u + 1] = clamp8(g);
            dst[dx * 4u + 2] = clamp8(b);
            dst[dx * 4u + 3] = alpha;
        }
    }
}

s32 cellVpostQueryAttr(const CellVpostCfgParam* cfgParam, CellVpostAttr* attr)
{
    const u32 cfg_ea = GUEST_EA(cfgParam);
    const u32 attr_ea = GUEST_EA(attr);
    if (!cfg_ea) return CELL_VPOST_ERROR_Q_ARG_CFG_NULL;
    if (!attr_ea) return CELL_VPOST_ERROR_Q_ARG_ATTR_NULL;

    /* RPCS3 and retail libraries report a small fixed workspace. */
    vm_write32(attr_ea + 0x00, 4u * 1024u * 1024u);
    ((u8*)vm_to_host(attr_ea))[4] = 0; /* delay */
    ((u8*)vm_to_host(attr_ea))[5] = 0;
    ((u8*)vm_to_host(attr_ea))[6] = 0;
    ((u8*)vm_to_host(attr_ea))[7] = 0;
    vm_write32(attr_ea + 0x08, 0x00260000u);
    vm_write32(attr_ea + 0x0c, 0x00280000u);
    return CELL_OK;
}

s32 cellVpostQuery(const CellVpostCfgParam* cfgParam, CellVpostAttr* attr)
{
    return cellVpostQueryAttr(cfgParam, attr);
}

s32 cellVpostOpen(const CellVpostCfgParam* cfgParam,
                  const CellVpostResource* resource,
                  CellVpostHandle* handle)
{
    const u32 cfg_ea = GUEST_EA(cfgParam);
    const u32 res_ea = GUEST_EA(resource);
    const u32 h_ea = GUEST_EA(handle);
    if (!cfg_ea) return CELL_VPOST_ERROR_O_ARG_CFG_NULL;
    if (!res_ea) return CELL_VPOST_ERROR_O_ARG_RSRC_NULL;
    if (!h_ea) return CELL_VPOST_ERROR_O_ARG_HDL_NULL;

    for (u32 i = 0; i < CELL_VPOST_HANDLE_MAX; ++i) {
        if (s_handles[i].in_use) continue;
        memset(&s_handles[i], 0, sizeof(s_handles[i]));
        s_handles[i].in_use = 1;
        load_cfg(&s_handles[i].cfg, cfg_ea);
        vm_write32(h_ea, i);
        fprintf(stderr,
                "[cellVpost] Open -> handle=%u inMax=%ux%u outMax=%ux%u outFmt=%d\n",
                i, s_handles[i].cfg.inMaxWidth, s_handles[i].cfg.inMaxHeight,
                s_handles[i].cfg.outMaxWidth, s_handles[i].cfg.outMaxHeight,
                s_handles[i].cfg.outPicFmt);
        return CELL_OK;
    }
    return CELL_VPOST_ERROR_O_ARG_HDL_NULL;
}

s32 cellVpostInit(const CellVpostCfgParam* cfgParam,
                  const CellVpostResource* resource,
                  CellVpostHandle* handle)
{
    return cellVpostOpen(cfgParam, resource, handle);
}

s32 cellVpostClose(CellVpostHandle handle)
{
    if (handle >= CELL_VPOST_HANDLE_MAX || !s_handles[handle].in_use)
        return CELL_VPOST_ERROR_C_ARG_HDL_INVALID;

    if (s_handles[handle].movie_mode) {
        s_handles[handle].movie_mode = 0;
        if (s_movie_users > 0) --s_movie_users;
        if (s_movie_users == 0) rsx_d3d12_backend_set_movie_mode(0);
    }
    fprintf(stderr, "[cellVpost] Close(handle=%u frames=%llu)\n", handle,
            (unsigned long long)s_handles[handle].exec_count);
    memset(&s_handles[handle], 0, sizeof(s_handles[handle]));
    return CELL_OK;
}

s32 cellVpostEnd(CellVpostHandle handle)
{
    return cellVpostClose(handle);
}

s32 cellVpostExec(CellVpostHandle handle,
                  const void* inPicBuf,
                  const CellVpostCtrlParam* ctrlParam,
                  void* outPicBuf,
                  CellVpostPictureInfo* picInfo)
{
    if (handle >= CELL_VPOST_HANDLE_MAX || !s_handles[handle].in_use)
        return CELL_VPOST_ERROR_E_ARG_HDL_INVALID;

    const u32 in_ea = GUEST_EA(inPicBuf);
    const u32 ctrl_ea = GUEST_EA(ctrlParam);
    const u32 out_ea = GUEST_EA(outPicBuf);
    const u32 info_ea = GUEST_EA(picInfo);
    if (!in_ea) return CELL_VPOST_ERROR_E_ARG_INPICBUF_NULL;
    if (!ctrl_ea) return CELL_VPOST_ERROR_E_ARG_CTRL_NULL;
    if (!out_ea) return CELL_VPOST_ERROR_E_ARG_OUTPICBUF_NULL;
    if (!info_ea) return CELL_VPOST_ERROR_E_ARG_PICINFO_NULL;

    const u32 iw = vm_read32(ctrl_ea + 0x0c);
    const u32 ih = vm_read32(ctrl_ea + 0x10);
    const s32 chroma = (s32)vm_read32(ctrl_ea + 0x14);
    const s32 quant  = (s32)vm_read32(ctrl_ea + 0x18);
    const s32 matrix = (s32)vm_read32(ctrl_ea + 0x1c);
    const u32 ow = vm_read32(ctrl_ea + 0x30);
    const u32 oh = vm_read32(ctrl_ea + 0x34);
    const u8 alpha = ((const u8*)vm_to_host(ctrl_ea))[0x48];
    const u64 user = vm_read64(ctrl_ea + 0x50);

    if (!iw || !ih || !ow || !oh || iw > 4096 || ih > 4096 ||
        ow > 4096 || oh > 4096 || (iw & 1u) || (ih & 1u)) {
        fprintf(stderr, "[cellVpost] Exec invalid dimensions %ux%u -> %ux%u\n",
                iw, ih, ow, oh);
        return CELL_VPOST_ERROR_E_ARG_CTRL_NULL;
    }

    const u8* in = (const u8*)vm_to_host(in_ea);
    u8* out = (u8*)vm_to_host(out_ea);
    const size_t ysz = (size_t)iw * ih;
    const u8* yp = in;
    const u8* up = in + ysz;
    const u8* vp = up + ysz / 4u;

    yuv_to_rgba(yp, up, vp, out, iw, ih, ow, oh, quant, matrix, alpha);
    store_pic_info(info_ea, iw, ih, ow, oh, chroma, quant, matrix, user);

    /* Stage into the renderer that actually owns eboot_port's window.
     * rsx_live_draw is not initialized by this runner, so sending frames there
     * only logged a call and then returned at g.ready == 0.  The active D3D12
     * backend consumes this staging buffer on its frame-clock thread. */
    rsx_d3d12_backend_submit_movie_rgba(out, ow, oh);
    /* Reassert this on every submitted frame.  WA2 does not reliably call
     * cellVpostClose between movie lifecycles, while the runtime deliberately
     * drops movie mode when vdispStart drains.  Making this idempotent avoids
     * a stale per-handle flag suppressing the next movie. */
    rsx_d3d12_backend_set_movie_mode(1);
    if (!s_handles[handle].movie_mode) {
        s_handles[handle].movie_mode = 1;
        ++s_movie_users;
    }

    const u64 n = ++s_handles[handle].exec_count;
    if (n <= 8 || (n % 120u) == 0) {
        fprintf(stderr,
                "[cellVpost] Exec #%llu %ux%u YUV420 -> %ux%u RGBA alpha=%u user=0x%llX backend-submit\n",
                (unsigned long long)n, iw, ih, ow, oh, alpha,
                (unsigned long long)user);
    }
    return CELL_OK;
}
