/*
 * ps3recomp - cellResc HLE
 *
 * RESC is a presentation path, not just state bookkeeping. White Album 2 is
 * one title that registers its render surface through RESC and presents with
 * cellRescSetConvertAndFlip(). The live RSX renderer consumes flip commands
 * from the guest FIFO, so this implementation mirrors the SDK-facing RESC
 * state and emits the same queue+flip method pair used by the GCM bridge.
 */
#include "cellResc.h"
#include "rsx_live_draw.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "ps3emu/guest_call.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RESC_SRC_COUNT 8

typedef struct resc_src_state {
    u32 format;
    u32 pitch;
    u16 width;
    u16 height;
    u32 offset;
    int valid;
} resc_src_state;

static int s_initialized;
static CellRescInitConfig s_config;
static CellRescDsts s_dsts[4];
static resc_src_state s_src[RESC_SRC_COUNT];
static u32 s_display_mode = CELL_RESC_1280x720;
static u32 s_flip_handler_opd;
static float s_aspect_h = 1.0f;
static float s_aspect_v = 1.0f;

static u32 resc_guest_ea(const void* host_ptr)
{
    uintptr_t base;
    uintptr_t p;
    uintptr_t delta;

    if (!host_ptr || !vm_base)
        return 0;
    base = (uintptr_t)vm_base;
    p = (uintptr_t)host_ptr;
    if (p < base)
        return 0;
    delta = p - base;
    if (delta > UINT32_MAX)
        return 0;
    return (u32)delta;
}

static int resc_dst_index(u32 mode)
{
    if (mode & CELL_RESC_720x576) return 1;
    if (mode & CELL_RESC_1280x720) return 2;
    if (mode & CELL_RESC_1920x1080) return 3;
    return 0;
}

/* Emit through the guest FIFO. This intentionally matches the runner's
 * context-aware _cellGcmSetFlipCommand bridge: queue buffer on head 1, then
 * issue the head-1 flip selecting the queued buffer. Keeping this in the FIFO
 * preserves draw -> flip ordering and lets yz_rsx_vblank_tick retire/present
 * the frame through the normal live-draw path. */
static s32 resc_append_flip(CellGcmContextData* context, u32 buffer_id)
{
    u32 context_ea = resc_guest_ea(context);
    u32 current;
    u32 end;
    static const u32 flip_words[4] = {
        0x0004E944u, 0u,
        0x0004E924u, 0x8000010Fu
    };
    u32 words[4];

    if (!context_ea || buffer_id >= RESC_SRC_COUNT)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    current = vm_read32(context_ea + 0x08u);
    end = vm_read32(context_ea + 0x04u);
    if (!current || current > UINT32_MAX - sizeof(words) ||
        current + sizeof(words) > end)
        return (s32)CELL_RESC_ERROR_GCM_FLIP_QUE_FULL;

    memcpy(words, flip_words, sizeof(words));
    words[1] = buffer_id;
    for (u32 i = 0; i < 4; ++i)
        vm_write32(current + i * 4u, words[i]);
    vm_write32(context_ea + 0x08u, current + (u32)sizeof(words));
    return CELL_OK;
}

s32 cellRescInit(const CellRescInitConfig* initConfig)
{
    u32 ea;

    if (s_initialized)
        return (s32)CELL_RESC_ERROR_REINITIALIZED;
    ea = resc_guest_ea(initConfig);
    if (!ea)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    s_config.size = vm_read32(ea + 0x00);
    s_config.resourcePolicy = vm_read32(ea + 0x04);
    s_config.supportModes = vm_read32(ea + 0x08);
    s_config.ratioMode = vm_read32(ea + 0x0C);
    s_config.palTemporalMode = vm_read32(ea + 0x10);
    s_config.interlaceMode = vm_read32(ea + 0x14);
    s_config.flipMode = vm_read32(ea + 0x18);

    memset(s_src, 0, sizeof(s_src));
    memset(s_dsts, 0, sizeof(s_dsts));
    s_display_mode = CELL_RESC_1280x720;
    s_flip_handler_opd = 0;
    s_aspect_h = 1.0f;
    s_aspect_v = 1.0f;
    s_initialized = 1;
    fprintf(stderr, "[cellResc] Init support=0x%X flip=%u\n",
            s_config.supportModes, s_config.flipMode);
    return CELL_OK;
}

s32 cellRescVideoOutResolutionId2RescBufferMode(u32 resolutionId, u32* bufferMode)
{
    u32 ea = resc_guest_ea(bufferMode);
    u32 mode;

    if (!ea)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    switch (resolutionId) {
    case 1: mode = CELL_RESC_1920x1080; break;
    case 2: mode = CELL_RESC_1280x720; break;
    case 4: mode = CELL_RESC_720x480; break;
    case 5: mode = CELL_RESC_720x576; break;
    default: mode = CELL_RESC_1280x720; break;
    }
    vm_write32(ea, mode);
    return CELL_OK;
}

s32 cellRescSetDsts(u32 bufferMode, const CellRescDsts* dsts)
{
    u32 ea;
    int idx;

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    ea = resc_guest_ea(dsts);
    if (!ea)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    idx = resc_dst_index(bufferMode);
    s_dsts[idx].format = vm_read32(ea + 0x00);
    s_dsts[idx].pitch = vm_read32(ea + 0x04);
    s_dsts[idx].heightAlign = vm_read32(ea + 0x08);
    return CELL_OK;
}

s32 cellRescSetDisplayMode(u32 bufferMode)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    if (!(bufferMode & (CELL_RESC_720x480 | CELL_RESC_720x576 |
                        CELL_RESC_1280x720 | CELL_RESC_1920x1080)))
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    s_display_mode = bufferMode;
    fprintf(stderr, "[cellResc] SetDisplayMode(0x%X)\n", bufferMode);
    return CELL_OK;
}

s32 cellRescAdjustAspectRatio(float horizontal, float vertical)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    s_aspect_h = horizontal;
    s_aspect_v = vertical;
    return CELL_OK;
}

s32 cellRescGetBufferSize(s32* colorBuffers, s32* vertexArray, s32* fragmentShader)
{
    u32 ea;

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    ea = resc_guest_ea(colorBuffers);
    if (ea) vm_write32(ea, 1920u * 1080u * 4u);
    ea = resc_guest_ea(vertexArray);
    if (ea) vm_write32(ea, 64u * 1024u);
    ea = resc_guest_ea(fragmentShader);
    if (ea) vm_write32(ea, 256u * 1024u);
    return CELL_OK;
}

s32 cellRescGcmSurface2RescSrc(const void* gcmSurface, CellRescSrc* rescSrc)
{
    u32 surf = resc_guest_ea(gcmSurface);
    u32 dst = resc_guest_ea(rescSrc);
    u32 format;
    u32 offset;
    u32 pitch;
    u16 width;
    u16 height;

    if (!surf || !dst)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* CellGcmSurface fields used by RESC. */
    format = (u32)vm_read8(surf + 0x01u);
    offset = vm_read32(surf + 0x08u);
    pitch = vm_read32(surf + 0x18u);
    width = vm_read16(surf + 0x34u);
    height = vm_read16(surf + 0x36u);

    /* Real SDK CellRescSrc layout: u32 format,pitch; u16 width,height; u32 offset. */
    vm_write32(dst + 0x00u, format);
    vm_write32(dst + 0x04u, pitch);
    vm_write16(dst + 0x08u, width);
    vm_write16(dst + 0x0Au, height);
    vm_write32(dst + 0x0Cu, offset);

    { static unsigned n;
      if (n++ < 12u)
          fprintf(stderr,
                  "[cellResc] GcmSurface2RescSrc off=0x%08X pitch=%u %ux%u fmt=%u\n",
                  offset, pitch, (unsigned)width, (unsigned)height, format); }
    return CELL_OK;
}

s32 cellRescSetSrc(s32 idx, const CellRescSrc* src)
{
    u32 ea;
    resc_src_state* out;
    s32 rc;

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    if (idx < 0 || idx >= RESC_SRC_COUNT)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    ea = resc_guest_ea(src);
    if (!ea)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    out = &s_src[idx];
    out->format = vm_read32(ea + 0x00u);
    out->pitch = vm_read32(ea + 0x04u);
    out->width = vm_read16(ea + 0x08u);
    out->height = vm_read16(ea + 0x0Au);
    out->offset = vm_read32(ea + 0x0Cu);
    out->valid = out->width && out->height && out->pitch;
    if (!out->valid)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* RESC owns the scanout registration for titles that never call
     * cellGcmSetDisplayBuffer themselves. Mirror it into both the GCM HLE and
     * the live renderer so the flip id resolves to this exact RSX surface. */
    rc = cellGcmSetDisplayBuffer((u32)idx, out->offset, out->pitch,
                                 out->width, out->height);
    if (rc != CELL_OK)
        return rc;
    rsx_live_draw_set_display_buffer((u32)idx, 0u, out->offset, out->pitch,
                                     out->width, out->height);

    { static unsigned n;
      if (n++ < 12u)
          fprintf(stderr,
                  "[cellResc] SetSrc(%d) display off=0x%08X pitch=%u %ux%u\n",
                  idx, out->offset, out->pitch,
                  (unsigned)out->width, (unsigned)out->height); }
    return CELL_OK;
}

s32 cellRescSetConvertAndFlip(CellGcmContextData* context, s32 idx)
{
    s32 rc;

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    if (idx < 0 || idx >= RESC_SRC_COUNT || !s_src[idx].valid)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    rc = resc_append_flip(context, (u32)idx);
    if (rc != CELL_OK)
        return rc;

    { static unsigned n;
      if (n++ < 16u)
          fprintf(stderr,
                  "[RESC-PRESENT] src=%d off=0x%08X -> FIFO E944/E924\n",
                  idx, s_src[idx].offset); }

    /* Match the known-good pre-regression RESC path: the title's registered
     * flip callback receives the completion edge it uses to advance its frame
     * state. The actual swap-chain present remains ordered by the FIFO/vblank. */
    if (s_flip_handler_opd)
        ps3_invoke_guest(s_flip_handler_opd, 1, 0, 0, 0, 0, 0, 0, 0);
    return CELL_OK;
}

s32 cellRescSetWaitFlip(CellGcmContextData* context)
{
    (void)context;
    /* Flip retirement is driven by yz_rsx_vblank_tick. Returning here avoids
     * introducing a second, out-of-order presentation path. */
    return CELL_OK;
}

s32 cellRescSetBufferAddress(const u32* colorBuffers, const u32* vertexArray,
                             const u32* fragmentShader)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    if (!colorBuffers || !vertexArray || !fragmentShader)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    return CELL_OK;
}

void cellRescSetFlipHandler(CellRescFlipHandler handler)
{
    s_flip_handler_opd = (u32)(uintptr_t)handler;
    cellGcmSetFlipHandler((CellGcmFlipHandler)(uintptr_t)s_flip_handler_opd);
    fprintf(stderr, "[cellResc] SetFlipHandler(opd=0x%08X)\n",
            s_flip_handler_opd);
}
