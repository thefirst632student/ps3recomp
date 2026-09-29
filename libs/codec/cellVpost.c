/*
 * ps3recomp - cellVpost HLE implementation
 *
 * Implements the video path used by PS3 movie players: planar YUV420 input,
 * optional scaling, and RGBA or planar-YUV420 output. The previous
 * implementation returned CELL_OK without touching outPicBuf, which made a
 * correctly decoded movie remain black.
 */

#include "cellVpost.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../runtime/ppu/ppu_memory.h"
#include "../guest_struct.h"

#ifdef PS3RECOMP_HAVE_FFMPEG
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#endif

typedef struct {
    int in_use;
    u32 inMaxWidth;
    u32 inMaxHeight;
    s32 inPicFmt;
    u32 outMaxWidth;
    u32 outMaxHeight;
    s32 outPicFmt;
#ifdef PS3RECOMP_HAVE_FFMPEG
    struct SwsContext* sws;
#endif
} VpostHandle;

static VpostHandle s_handles[CELL_VPOST_HANDLE_MAX];
static unsigned s_exec_count;

/* ABI offsets. Keep guest memory access explicit: CellVpostCtrlParam contains
 * a byte field and a u64, so guest_struct_load() is not suitable for it. */
enum {
    CTRL_EXEC_TYPE       = 0x00,
    CTRL_SCALER_TYPE     = 0x04,
    CTRL_IPC_TYPE        = 0x08,
    CTRL_IN_WIDTH        = 0x0C,
    CTRL_IN_HEIGHT       = 0x10,
    CTRL_IN_CHROMA       = 0x14,
    CTRL_IN_QUANT        = 0x18,
    CTRL_IN_MATRIX       = 0x1C,
    CTRL_IN_WIN_X        = 0x20,
    CTRL_IN_WIN_Y        = 0x24,
    CTRL_IN_WIN_W        = 0x28,
    CTRL_IN_WIN_H        = 0x2C,
    CTRL_OUT_WIDTH       = 0x30,
    CTRL_OUT_HEIGHT      = 0x34,
    CTRL_OUT_WIN_X       = 0x38,
    CTRL_OUT_WIN_Y       = 0x3C,
    CTRL_OUT_WIN_W       = 0x40,
    CTRL_OUT_WIN_H       = 0x44,
    CTRL_OUT_ALPHA       = 0x48,
    CTRL_USER_DATA       = 0x50,
    CTRL_RESERVED1       = 0x58,
    CTRL_RESERVED2       = 0x5C
};

enum {
    PIC_IN_WIDTH         = 0x00,
    PIC_IN_HEIGHT        = 0x04,
    PIC_IN_DEPTH         = 0x08,
    PIC_IN_SCAN          = 0x0C,
    PIC_IN_FMT           = 0x10,
    PIC_IN_CHROMA        = 0x14,
    PIC_IN_STRUCT        = 0x18,
    PIC_IN_QUANT         = 0x1C,
    PIC_IN_MATRIX        = 0x20,
    PIC_OUT_WIDTH        = 0x24,
    PIC_OUT_HEIGHT       = 0x28,
    PIC_OUT_DEPTH        = 0x2C,
    PIC_OUT_SCAN         = 0x30,
    PIC_OUT_FMT          = 0x34,
    PIC_OUT_CHROMA       = 0x38,
    PIC_OUT_STRUCT       = 0x3C,
    PIC_OUT_QUANT        = 0x40,
    PIC_OUT_MATRIX       = 0x44,
    PIC_USER_DATA        = 0x48,
    PIC_RESERVED1        = 0x50,
    PIC_RESERVED2        = 0x54
};

static int valid_dim(u32 v)
{
    return v >= 16 && v <= 4096;
}

static void handle_reset(VpostHandle* h)
{
#ifdef PS3RECOMP_HAVE_FFMPEG
    if (h->sws) sws_freeContext(h->sws);
#endif
    memset(h, 0, sizeof(*h));
}

static void read_cfg(VpostHandle* h, u32 ea)
{
    h->inMaxWidth  = vm_read32(ea + 0x00);
    h->inMaxHeight = vm_read32(ea + 0x04);
    h->inPicFmt    = (s32)vm_read32(ea + 0x0C);
    h->outMaxWidth = vm_read32(ea + 0x10);
    h->outMaxHeight= vm_read32(ea + 0x14);
    h->outPicFmt   = (s32)vm_read32(ea + 0x1C);
}

s32 cellVpostQuery(const CellVpostCfgParam* cfgParam, u32* memSize)
{
    if (!cfgParam) return (s32)CELL_VPOST_ERROR_Q_ARG_CFG_NULL;
    if (!memSize) return (s32)CELL_VPOST_ERROR_Q_ARG_ATTR_NULL;
    vm_write32(GUEST_EA(memSize), 4u * 1024u * 1024u);
    return CELL_OK;
}

s32 cellVpostQueryAttr(const CellVpostCfgParam* cfgParam, CellVpostAttr* attr)
{
    if (!cfgParam) return (s32)CELL_VPOST_ERROR_Q_ARG_CFG_NULL;
    if (!attr) return (s32)CELL_VPOST_ERROR_Q_ARG_ATTR_NULL;

    const u32 a = GUEST_EA(attr);
    vm_write32(a + 0x00, 4u * 1024u * 1024u);
    vm_write8 (a + 0x04, 0);
    vm_write8 (a + 0x05, 0);
    vm_write8 (a + 0x06, 0);
    vm_write8 (a + 0x07, 0);
    vm_write32(a + 0x08, 0x00280000u);
    vm_write32(a + 0x0C, 0x00260000u);
    return CELL_OK;
}

s32 cellVpostInit(const CellVpostCfgParam* cfgParam,
                  const CellVpostResource* resource,
                  CellVpostHandle* handle)
{
    if (!cfgParam) return (s32)CELL_VPOST_ERROR_O_ARG_CFG_NULL;
    if (!resource) return (s32)CELL_VPOST_ERROR_O_ARG_RSRC_NULL;
    if (!handle) return (s32)CELL_VPOST_ERROR_O_ARG_HDL_NULL;

    const u32 cfg = GUEST_EA(cfgParam);
    const u32 inFmt = vm_read32(cfg + 0x0C);
    const u32 outFmt = vm_read32(cfg + 0x1C);
    const u32 iw = vm_read32(cfg + 0x00);
    const u32 ih = vm_read32(cfg + 0x04);
    const u32 ow = vm_read32(cfg + 0x10);
    const u32 oh = vm_read32(cfg + 0x14);

    if (inFmt != CELL_VPOST_PIC_FMT_IN_YUV420_PLANAR ||
        outFmt > CELL_VPOST_PIC_FMT_OUT_YUV420_PLANAR ||
        !valid_dim(iw) || !valid_dim(ih) || !valid_dim(ow) || !valid_dim(oh)) {
        fprintf(stderr, "[cellVpost] Open invalid cfg in=%ux%u fmt=%u out=%ux%u fmt=%u\n",
                iw, ih, inFmt, ow, oh, outFmt);
        return (s32)CELL_VPOST_ERROR_O_ARG_CFG_INVALID;
    }

    for (u32 i = 0; i < CELL_VPOST_HANDLE_MAX; ++i) {
        if (!s_handles[i].in_use) {
            VpostHandle* h = &s_handles[i];
            handle_reset(h);
            h->in_use = 1;
            read_cfg(h, cfg);
            vm_write32(GUEST_EA(handle), i);
            printf("[cellVpost] Open -> handle=%u inMax=%ux%u outMax=%ux%u outFmt=%d\n",
                   i, h->inMaxWidth, h->inMaxHeight,
                   h->outMaxWidth, h->outMaxHeight, h->outPicFmt);
            return CELL_OK;
        }
    }
    return (s32)CELL_VPOST_ERROR_O_ARG_RSRC_INVALID;
}

s32 cellVpostOpen(const CellVpostCfgParam* cfgParam,
                  const CellVpostResource* resource,
                  CellVpostHandle* handle)
{
    return cellVpostInit(cfgParam, resource, handle);
}

s32 cellVpostEnd(CellVpostHandle handle)
{
    if (handle >= CELL_VPOST_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_VPOST_ERROR_C_ARG_HDL_INVALID;
    printf("[cellVpost] Close(%u)\n", handle);
    handle_reset(&s_handles[handle]);
    return CELL_OK;
}

s32 cellVpostClose(CellVpostHandle handle)
{
    return cellVpostEnd(handle);
}

/* Nearest-neighbour planar scaler used when FFmpeg was disabled at build time.
 * Keeping this fallback makes cellVpost correct even in minimal builds. */
static void scale_plane_nn(const u8* src, u32 sw, u32 sh, u32 sstride,
                           u8* dst, u32 dw, u32 dh, u32 dstride)
{
    for (u32 y = 0; y < dh; ++y) {
        u32 sy = (u32)(((u64)y * sh) / dh);
        const u8* s = src + (size_t)sy * sstride;
        u8* d = dst + (size_t)y * dstride;
        for (u32 x = 0; x < dw; ++x) {
            u32 sx = (u32)(((u64)x * sw) / dw);
            d[x] = s[sx];
        }
    }
}

static inline u8 clamp8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (u8)v;
}

static void yuv420_to_rgba_nn(const u8* src, u32 sw, u32 sh,
                              u8* dst, u32 dw, u32 dh,
                              int matrix, int quant, u8 alpha)
{
    const u8* py = src;
    const u8* pu = py + (size_t)sw * sh;
    const u8* pv = pu + (size_t)(sw / 2) * (sh / 2);

    for (u32 y = 0; y < dh; ++y) {
        u32 sy = (u32)(((u64)y * sh) / dh);
        for (u32 x = 0; x < dw; ++x) {
            u32 sx = (u32)(((u64)x * sw) / dw);
            int Y = py[(size_t)sy * sw + sx];
            int U = pu[(size_t)(sy / 2) * (sw / 2) + sx / 2] - 128;
            int V = pv[(size_t)(sy / 2) * (sw / 2) + sx / 2] - 128;
            int r, g, b;

            if (quant == CELL_VPOST_QUANT_RANGE_BROADCAST) {
                int C = Y - 16;
                if (C < 0) C = 0;
                if (matrix == CELL_VPOST_COLOR_MATRIX_BT709) {
                    r = (298 * C + 459 * V + 128) >> 8;
                    g = (298 * C -  55 * U - 136 * V + 128) >> 8;
                    b = (298 * C + 541 * U + 128) >> 8;
                } else {
                    r = (298 * C + 409 * V + 128) >> 8;
                    g = (298 * C - 100 * U - 208 * V + 128) >> 8;
                    b = (298 * C + 516 * U + 128) >> 8;
                }
            } else {
                if (matrix == CELL_VPOST_COLOR_MATRIX_BT709) {
                    r = Y + ((403 * V) >> 8);
                    g = Y - (( 48 * U + 120 * V) >> 8);
                    b = Y + ((475 * U) >> 8);
                } else {
                    r = Y + ((359 * V) >> 8);
                    g = Y - (( 88 * U + 183 * V) >> 8);
                    b = Y + ((454 * U) >> 8);
                }
            }

            size_t p = ((size_t)y * dw + x) * 4;
            dst[p + 0] = clamp8(r);
            dst[p + 1] = clamp8(g);
            dst[p + 2] = clamp8(b);
            dst[p + 3] = alpha;
        }
    }
}

static void write_pic_info(u32 pic, u32 w, u32 h, u32 ow, u32 oh,
                           s32 chroma, s32 quant, s32 matrix,
                           s32 outFmt, u64 userData)
{
    vm_write32(pic + PIC_IN_WIDTH,   w);
    vm_write32(pic + PIC_IN_HEIGHT,  h);
    vm_write32(pic + PIC_IN_DEPTH,   CELL_VPOST_PIC_DEPTH_8);
    vm_write32(pic + PIC_IN_SCAN,    CELL_VPOST_SCAN_TYPE_P);
    vm_write32(pic + PIC_IN_FMT,     CELL_VPOST_PIC_FMT_IN_YUV420_PLANAR);
    vm_write32(pic + PIC_IN_CHROMA,  (u32)chroma);
    vm_write32(pic + PIC_IN_STRUCT,  CELL_VPOST_PIC_STRUCT_PFRM);
    vm_write32(pic + PIC_IN_QUANT,   (u32)quant);
    vm_write32(pic + PIC_IN_MATRIX,  (u32)matrix);
    vm_write32(pic + PIC_OUT_WIDTH,  ow);
    vm_write32(pic + PIC_OUT_HEIGHT, oh);
    vm_write32(pic + PIC_OUT_DEPTH,  CELL_VPOST_PIC_DEPTH_8);
    vm_write32(pic + PIC_OUT_SCAN,   CELL_VPOST_SCAN_TYPE_P);
    vm_write32(pic + PIC_OUT_FMT,    (u32)outFmt);
    vm_write32(pic + PIC_OUT_CHROMA, (u32)chroma);
    vm_write32(pic + PIC_OUT_STRUCT, CELL_VPOST_PIC_STRUCT_PFRM);
    vm_write32(pic + PIC_OUT_QUANT,  (u32)quant);
    vm_write32(pic + PIC_OUT_MATRIX, (u32)matrix);
    vm_write64(pic + PIC_USER_DATA, userData);
    vm_write32(pic + PIC_RESERVED1, 0);
    vm_write32(pic + PIC_RESERVED2, 0);
}

s32 cellVpostExec(CellVpostHandle handle,
                  const void* inPicBuf,
                  const CellVpostCtrlParam* ctrlParam,
                  void* outPicBuf,
                  CellVpostPictureInfo* picInfo)
{
    if (handle >= CELL_VPOST_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_VPOST_ERROR_E_ARG_HDL_INVALID;
    if (!inPicBuf) return (s32)CELL_VPOST_ERROR_E_ARG_INPICBUF_NULL;
    if (!ctrlParam) return (s32)CELL_VPOST_ERROR_E_ARG_CTRL_NULL;
    if (!outPicBuf) return (s32)CELL_VPOST_ERROR_E_ARG_OUTPICBUF_NULL;
    if (!picInfo) return (s32)CELL_VPOST_ERROR_E_ARG_PICINFO_NULL;

    VpostHandle* vh = &s_handles[handle];
    const u32 c = GUEST_EA(ctrlParam);
    const u32 w = vm_read32(c + CTRL_IN_WIDTH);
    const u32 h = vm_read32(c + CTRL_IN_HEIGHT);
    u32 ow = vm_read32(c + CTRL_OUT_WIDTH);
    u32 oh = vm_read32(c + CTRL_OUT_HEIGHT);
    const s32 chroma = (s32)vm_read32(c + CTRL_IN_CHROMA);
    const s32 quant  = (s32)vm_read32(c + CTRL_IN_QUANT);
    const s32 matrix = (s32)vm_read32(c + CTRL_IN_MATRIX);
    const u8 alpha = vm_read8(c + CTRL_OUT_ALPHA);
    const u64 userData = vm_read64(c + CTRL_USER_DATA);

    if (!ow) ow = w;
    if (!oh) oh = h;
    if (!valid_dim(w) || !valid_dim(h) || !valid_dim(ow) || !valid_dim(oh) ||
        (w & 1) || (h & 1) || (ow & 1) || (oh & 1) ||
        w > vh->inMaxWidth || h > vh->inMaxHeight ||
        ow > vh->outMaxWidth || oh > vh->outMaxHeight) {
        fprintf(stderr, "[cellVpost] Exec invalid dims %ux%u -> %ux%u (max %ux%u -> %ux%u)\n",
                w, h, ow, oh, vh->inMaxWidth, vh->inMaxHeight,
                vh->outMaxWidth, vh->outMaxHeight);
        return (s32)CELL_VPOST_ERROR_E_ARG_CTRL_INVALID;
    }

    const u8* src = GUEST_PTR(inPicBuf, const u8*);
    u8* dst = GUEST_PTR(outPicBuf, u8*);
    const s32 outFmt = vh->outPicFmt;

#ifdef PS3RECOMP_HAVE_FFMPEG
    {
        enum AVPixelFormat dstFmt = outFmt == CELL_VPOST_PIC_FMT_OUT_YUV420_PLANAR
                                  ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_RGBA;
        vh->sws = sws_getCachedContext(vh->sws,
                                       (int)w, (int)h, AV_PIX_FMT_YUV420P,
                                       (int)ow, (int)oh, dstFmt,
                                       SWS_BILINEAR, NULL, NULL, NULL);
        if (!vh->sws)
            return (s32)CELL_VPOST_ERROR_E_ARG_CTRL_INVALID;

        /* Match the control block's matrix/range where libswscale supports it. */
        {
            const int* coeff = sws_getCoefficients(matrix == CELL_VPOST_COLOR_MATRIX_BT709
                                                  ? SWS_CS_ITU709 : SWS_CS_ITU601);
            int srcRange = quant == CELL_VPOST_QUANT_RANGE_FULL ? 1 : 0;
            int dstRange = srcRange;
            (void)sws_setColorspaceDetails(vh->sws, coeff, srcRange,
                                           coeff, dstRange, 0, 1 << 16, 1 << 16);
        }

        const u8* inData[4] = {
            src,
            src + (size_t)w * h,
            src + (size_t)w * h + (size_t)(w / 2) * (h / 2),
            NULL
        };
        int inStride[4] = { (int)w, (int)(w / 2), (int)(w / 2), 0 };
        u8* outData[4] = { dst, NULL, NULL, NULL };
        int outStride[4] = { 0, 0, 0, 0 };
        if (dstFmt == AV_PIX_FMT_RGBA) {
            outStride[0] = (int)ow * 4;
        } else {
            outData[1] = dst + (size_t)ow * oh;
            outData[2] = outData[1] + (size_t)(ow / 2) * (oh / 2);
            outStride[0] = (int)ow;
            outStride[1] = outStride[2] = (int)(ow / 2);
        }
        if (sws_scale(vh->sws, inData, inStride, 0, (int)h,
                      outData, outStride) <= 0)
            return (s32)CELL_VPOST_ERROR_E_ARG_CTRL_INVALID;

        if (dstFmt == AV_PIX_FMT_RGBA && alpha != 255) {
            size_t pixels = (size_t)ow * oh;
            for (size_t i = 0; i < pixels; ++i) dst[i * 4 + 3] = alpha;
        }
    }
#else
    if (outFmt == CELL_VPOST_PIC_FMT_OUT_YUV420_PLANAR) {
        const u8* sy = src;
        const u8* su = sy + (size_t)w * h;
        const u8* sv = su + (size_t)(w / 2) * (h / 2);
        u8* dy = dst;
        u8* du = dy + (size_t)ow * oh;
        u8* dv = du + (size_t)(ow / 2) * (oh / 2);
        scale_plane_nn(sy, w, h, w, dy, ow, oh, ow);
        scale_plane_nn(su, w / 2, h / 2, w / 2, du, ow / 2, oh / 2, ow / 2);
        scale_plane_nn(sv, w / 2, h / 2, w / 2, dv, ow / 2, oh / 2, ow / 2);
    } else {
        yuv420_to_rgba_nn(src, w, h, dst, ow, oh, matrix, quant, alpha);
    }
#endif

    write_pic_info(GUEST_EA(picInfo), w, h, ow, oh,
                   chroma, quant, matrix, outFmt, userData);

    if (s_exec_count < 12 || (s_exec_count % 120) == 0) {
        printf("[cellVpost] Exec #%u %ux%u YUV420 -> %ux%u %s alpha=%u user=0x%llX\n",
               s_exec_count + 1, w, h, ow, oh,
               outFmt == CELL_VPOST_PIC_FMT_OUT_YUV420_PLANAR ? "YUV420" : "RGBA",
               (unsigned)alpha, (unsigned long long)userData);
    }
    ++s_exec_count;
    return CELL_OK;
}
