#include <cstdio>
#include <cstring>
#include <cstdint>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

// For SPURS workload registry
#include "../runtime/spu/spu_context.h"
#include "../runtime/spu/spu_workload.h"

extern "C" void ps3_hle_register(unsigned int nid, const char* name, void* handler);
extern "C" s32 cellSpursEventFlagSet(void* eventFlag, u16 bits);
extern uint8_t* vm_base;

// Missing NIDs
static s32 _cellSpursLFQueuePushBody(u64 queue_ea, u64 buffer_ea, u64 size) {
    return CELL_OK;
}

static s32 cellRescVideoOutResolutionId2RescBufferMode(u32 resId, u32* mode) {
    if (mode) *mode = 0;
    return CELL_OK;
}

static s32 _sys_spu_printf_initialize(u32 unk1, u32 unk2) {
    return CELL_OK;
}

static s32 cellAudioSetPortLevel(u32 portNum, float volume) {
    return CELL_OK;
}

// SPURS Fallback for fp=0x1AF3B10ECB1562C3
static void synth2_fallback(spu_context* ctx) {
    printf("[fallback] synth2 SPURS workload running!\n");
    
    // Unblock the PPU waiting for this SPURS task
    // The exact flag is hardcoded based on the log for now
    cellSpursEventFlagSet((void*)(uintptr_t)0x01178880, 0x0001);
}

extern "C" void ps3_load_prx_modules(void)
{
    // Register missing NIDs
    ps3_hle_register(0x8A85674D, "_cellSpursLFQueuePushBody", (void*)_cellSpursLFQueuePushBody);
    ps3_hle_register(0xD1CA0503, "cellRescVideoOutResolutionId2RescBufferMode", (void*)cellRescVideoOutResolutionId2RescBufferMode);
    ps3_hle_register(0x45FE2FCE, "_sys_spu_printf_initialize", (void*)_sys_spu_printf_initialize);
    ps3_hle_register(0x56DFE179, "cellAudioSetPortLevel", (void*)cellAudioSetPortLevel);
    
    // Register SPU Workload
    spu_workload_register(0x1AF3B10ECB1562C3ULL, synth2_fallback, "synth2");
}
