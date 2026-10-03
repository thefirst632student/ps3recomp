/*
 * ps3recomp - cellResc HLE implementation
 *
 * Resolution scaling stub. Since the recompiled game renders through
 * the host GPU, actual rescaling is handled by the host graphics API.
 * This module tracks state so game code that queries RESC works correctly.
 */

#include "cellResc.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: translate + byte-swap */
#include "../guest_struct.h"                /* guest_struct_load: BE struct word-swap */
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_initialized = 0;
static CellRescInitConfig s_config;
static u32 s_display_mode = CELL_RESC_1280x720;
static CellRescSrc s_src[8]; /* up to 8 color buffers */
static CellRescDsts s_dsts[4]; /* one per display mode */
static u32 s_flip_target = 0;   /* display buffer the next convert-and-flip presents */
static float s_aspect_h = 1.0f;
static float s_aspect_v = 1.0f;
static float s_pal_ratio = 0.5f;

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellRescInit(const CellRescInitConfig* initConfig)
{
    printf("[cellResc] Init()\n");

    if (s_initialized)
        return (s32)CELL_RESC_ERROR_REINITIALIZED;

    if (!initConfig)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    guest_struct_load(&s_config, GUEST_EA(initConfig), sizeof(s_config));
    memset(s_src, 0, sizeof(s_src));
    memset(s_dsts, 0, sizeof(s_dsts));
    s_flip_target = 0;
    s_aspect_h = 1.0f;
    s_aspect_v = 1.0f;
    s_initialized = 1;
    return CELL_OK;
}

void cellRescExit(void)
{
    printf("[cellResc] Exit()\n");
    s_initialized = 0;
}

s32 cellRescSetDisplayMode(u32 displayMode)
{
    printf("[cellResc] SetDisplayMode(0x%x)\n", displayMode);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    s_display_mode = displayMode;
    return CELL_OK;
}

s32 cellRescGetNumColorBuffers(u32 displayMode, u32 palTemporalMode, u32* numBufs)
{
    (void)displayMode;

    if (!numBufs)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* PAL interpolation modes need extra buffers */
    switch (palTemporalMode) {
    case CELL_RESC_PAL_60_INTERPOLATE:
    case CELL_RESC_PAL_60_INTERPOLATE_30_DROP:
    case CELL_RESC_PAL_60_INTERPOLATE_DROP_FLEXIBLE:
        vm_write32((uint32_t)(uintptr_t)numBufs, 6);
        break;
    default:
        vm_write32((uint32_t)(uintptr_t)numBufs, 2);
        break;
    }
    return CELL_OK;
}

s32 cellRescVideoOutResolutionId2RescBufferMode(u32 resolutionId, u32* bufferMode)
{
    if (!bufferMode)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    u32 mode = CELL_RESC_1280x720;
    switch (resolutionId) {
    case 1: /* CELL_VIDEO_OUT_RESOLUTION_1080 */
        mode = CELL_RESC_1920x1080;
        break;
    case 2: /* CELL_VIDEO_OUT_RESOLUTION_720 */
        mode = CELL_RESC_1280x720;
        break;
    case 4: /* CELL_VIDEO_OUT_RESOLUTION_480 */
        mode = CELL_RESC_720x480;
        break;
    case 5: /* CELL_VIDEO_OUT_RESOLUTION_576 */
        mode = CELL_RESC_720x576;
        break;
    default:
        mode = CELL_RESC_1280x720;
        break;
    }

    vm_write32((uint32_t)(uintptr_t)bufferMode, mode);
    return CELL_OK;
}

s32 cellRescGetBufferSize(u32* colorBufSize, u32* vertexBufSize, u32* fragmentBufSize)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    /* Provide reasonable buffer sizes for state tracking */
    if (colorBufSize)
        vm_write32((uint32_t)(uintptr_t)colorBufSize, 1920 * 1080 * 4); /* RGBA 1080p */
    if (vertexBufSize)
        vm_write32((uint32_t)(uintptr_t)vertexBufSize, 64 * 1024); /* 64KB vertex buffer */
    if (fragmentBufSize)
        vm_write32((uint32_t)(uintptr_t)fragmentBufSize, 256 * 1024); /* 256KB fragment shader */

    return CELL_OK;
}

s32 cellRescSetBufferAddress(void* colorBuf, void* vertexBuf, void* fragmentBuf)
{
    (void)colorBuf;
    (void)vertexBuf;
    (void)fragmentBuf;

    printf("[cellResc] SetBufferAddress()\n");

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    /* In host-GPU mode we don't use these buffers directly */
    return CELL_OK;
}

s32 cellRescGcmSurface2RescSrc(const void* surface, CellRescSrc* src)
{
    if (!surface || !src)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    uint32_t surf_ea = GUEST_EA(surface);
    uint32_t src_ea  = GUEST_EA(src);

    uint32_t format = (uint32_t)vm_read8(surf_ea + 0x01);
    uint32_t offset = vm_read32(surf_ea + 0x08);
    uint32_t pitch  = vm_read32(surf_ea + 0x18);
    uint32_t width  = (uint32_t)vm_read16(surf_ea + 0x34);
    uint32_t height = (uint32_t)vm_read16(surf_ea + 0x36);

    {
        static int n = 0;
        if (n++ < 8) {
            printf("[cellResc] GcmSurface2RescSrc: surf=0x%08X off=0x%08X pitch=%u %ux%u fmt=%u -> src=0x%08X\n",
                   surf_ea, offset, pitch, width, height, format, src_ea);
            printf("[cellResc] surf dwords: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                   vm_read32(surf_ea + 0x00), vm_read32(surf_ea + 0x04),
                   vm_read32(surf_ea + 0x08), vm_read32(surf_ea + 0x0C),
                   vm_read32(surf_ea + 0x10), vm_read32(surf_ea + 0x14),
                   vm_read32(surf_ea + 0x18), vm_read32(surf_ea + 0x1C));
        }
    }

    vm_write32(src_ea + 0x00, format);
    vm_write32(src_ea + 0x04, pitch);
    vm_write32(src_ea + 0x08, width);
    vm_write32(src_ea + 0x0C, height);
    vm_write32(src_ea + 0x10, offset);

    return CELL_OK;
}

s32 cellRescSetSrc(s32 index, const CellRescSrc* src)
{
    {
        static int n = 0;
        if (n++ < 8)
            printf("[cellResc] SetSrc(index=%d)\n", index);
    }

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!src || index < 0 || index >= 8)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    guest_struct_load(&s_src[index], GUEST_EA(src), sizeof(s_src[index]));

    if (s_src[index].width > 0 && s_src[index].height > 0) {
        extern s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                                           u32 width, u32 height);
        cellGcmSetDisplayBuffer((u32)index, s_src[index].offset,
                                s_src[index].pitch,
                                s_src[index].width, s_src[index].height);
    }
    return CELL_OK;
}

s32 cellRescSetDsts(u32 displayMode, const CellRescDsts* dsts)
{
    {
        static int n = 0;
        if (n++ < 8)
            printf("[cellResc] SetDsts(mode=0x%x)\n", displayMode);
    }

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!dsts)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* Map display mode to index */
    int idx = 0;
    if (displayMode & CELL_RESC_720x576) idx = 1;
    else if (displayMode & CELL_RESC_1280x720) idx = 2;
    else if (displayMode & CELL_RESC_1920x1080) idx = 3;

    guest_struct_load(&s_dsts[idx], GUEST_EA(dsts), sizeof(s_dsts[idx]));
    return CELL_OK;
}

s32 cellRescSetConvertAndFlip(void* context, s32 index)
{
    (void)context;
    if (index < 0 || index >= 8)
        index = 0;

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    /* This is a real flip, not bookkeeping. A title that presents through RESC
     * -- Virtua Fighter 5 does -- renders into an off-screen surface and asks
     * RESC to scale it into a display buffer and show it. Tracking a counter
     * here and returning CELL_OK meant no flip ever reached cellGcm, so the
     * title's frame loop never closed: nothing retired, and it kept appending
     * to a command buffer it could never reclaim.
     *
     * ponytail: the convert half is deliberately not emitted -- the backend
     * already presents at the output resolution, so RESC's scale is a no-op for
     * us. What must happen is the flip. Rotate through the display buffers the
     * title registered, the way RESC's own double/triple buffering does. Emit a
     * real conversion draw if a title ever needs RESC's PAL/interlace modes
     * rather than a plain scale. */
    extern s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                                       u32 width, u32 height);
    extern s32 cellGcmSetFlipCommandForResc(u32 bufferId);
    extern u32 cellGcm_display_buffer_count(void);
    u32 nbuf = cellGcm_display_buffer_count();

    /* RESC-only games (White Album 2, etc.) never call cellGcmSetDisplayBuffer
     * themselves. On real hardware RESC internally allocates output buffers and
     * manages the flip. Here we auto-register each RESC source surface as a GCM
     * display buffer so:
     *   (a) cellGcmOffsetIsDisplay returns true for the render target offset,
     *       which makes the D3D12 backend classify draws as on-screen, and
     *   (b) cellGcmSetFlipCommand has a valid buffer to flip. */
    if (nbuf == 0) {
        for (int i = 0; i < 8; i++) {
            if (s_src[i].width > 0 && s_src[i].height > 0) {
                cellGcmSetDisplayBuffer((u32)i, s_src[i].offset,
                                        s_src[i].pitch,
                                        s_src[i].width, s_src[i].height);
            }
        }
        nbuf = cellGcm_display_buffer_count();
        printf("[cellResc] auto-registered %u RESC source(s) as display buffer(s)\n", nbuf);
    }

    if (nbuf) {
        /* A RESC flip handler is a completion notification.  Do not use the
         * generic GCM path's legacy request-time callback: WA2's registered
         * handler (OPD 0x00140C60 -> func_00070E24) sets 0x003F3B8C, and the
         * render loop waits on/clears that byte before submitting the next
         * frame.  Signalling it here makes the wait a no-op and allows the
         * guest to queue multiple frames before the previous one is actually
         * presented. */
        cellGcmSetFlipCommandForResc(s_flip_target % nbuf);
        s_flip_target = (u32)((s_flip_target + 1) % nbuf);
    }

    { static int n = 0;
      if (n++ < 8)
          printf("[cellResc] SetConvertAndFlip(src=%d) -> flip, %u display buffer(s)\n",
                 index, nbuf); }

    return CELL_OK;
}

s32 cellRescSetWaitFlip(void* context)
{
    (void)context;
    extern void cellGcmSetWaitFlip(void);
    cellGcmSetWaitFlip();
    return CELL_OK;
}

s32 cellRescSetFlipHandler(void (*handler)(u32))
{
    extern void cellGcmSetFlipHandler(void (*)(u32));
    printf("[cellResc] SetFlipHandler(opd=0x%08X)\n", (u32)(uintptr_t)handler);
    cellGcmSetFlipHandler(handler);
    return CELL_OK;
}

s32 cellRescSetVBlankHandler(void (*handler)(u32))
{
    extern void cellGcmSetVBlankHandler(void (*)(u32));
    printf("[cellResc] SetVBlankHandler(opd=0x%08X)\n", (u32)(uintptr_t)handler);
    cellGcmSetVBlankHandler(handler);
    return CELL_OK;
}

s32 cellRescGetDisplayMode(u32* displayMode)
{
    if (!displayMode)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    vm_write32((uint32_t)(uintptr_t)displayMode, s_display_mode);
    return CELL_OK;
}

s32 cellRescGetLastFlipTime(u64* time)
{
    extern u64 cellGcmGetLastFlipTime(void);
    if (!time)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    vm_write64((uint32_t)(uintptr_t)time, cellGcmGetLastFlipTime());
    return CELL_OK;
}

void cellRescResetFlipStatus(void)
{
    extern void cellGcmResetFlipStatus(void);
    cellGcmResetFlipStatus();
}

s32 cellRescGetFlipStatus(void)
{
    extern u32 cellGcmGetFlipStatus(void);
    return (s32)cellGcmGetFlipStatus();
}

s32 cellRescSetPalInterpolateDropFlexRatio(float ratio)
{
    printf("[cellResc] SetPalInterpolateDropFlexRatio(%.2f)\n", ratio);
    s_pal_ratio = ratio;
    return CELL_OK;
}

s32 cellRescCreateInterlaceTable(void* buf, float ea, u32 tableLen, s32 depth)
{
    (void)ea;
    (void)depth;

    printf("[cellResc] CreateInterlaceTable(len=%u, depth=%d)\n", tableLen, depth);

    if (!buf)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* Fill with simple linear interpolation weights */
    float* table = (float*)buf;
    for (u32 i = 0; i < tableLen; i++)
        table[i] = (float)i / (float)(tableLen > 1 ? tableLen - 1 : 1);

    return CELL_OK;
}

s32 cellRescAdjustAspectRatio(float horizontal, float vertical)
{
    /* WA2 calls this every frame with the same values.  Keep the diagnostic on
     * state changes without turning stdout traffic into render cost. */
    static int s_aspect_logged = 0;
    static float s_log_h = 0.0f, s_log_v = 0.0f;
    if (!s_aspect_logged || horizontal != s_log_h || vertical != s_log_v) {
        printf("[cellResc] AdjustAspectRatio(h=%.2f, v=%.2f)\n", horizontal, vertical);
        s_aspect_logged = 1;
        s_log_h = horizontal; s_log_v = vertical;
    }

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    s_aspect_h = horizontal;
    s_aspect_v = vertical;
    return CELL_OK;
}
