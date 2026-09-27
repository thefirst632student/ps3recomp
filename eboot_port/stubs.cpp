#include <cstdio>
#include <cstring>
#include <cstdint>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

// For SPURS workload registry
#include "spu_context.h"
#include "spu_workload.h"


extern "C" void ps3_hle_register(unsigned int nid, const char* name, void* handler);
extern "C" s32 cellSpursEventFlagSet(void* eventFlag, u16 bits);

extern "C" {
    s32 cellRescGcmSurface2RescSrc(const void* surface, void* src);
    s32 cellRescSetWaitFlip(void* context);
    s32 cellRescVideoOutResolutionId2RescBufferMode(u32 resolutionId, u32* bufferMode);
    s32 cellAudioSetPortLevel(u32 portNum, float level);
    s32 _sys_spu_printf_initialize(const char* name, int prio);
    s32 _cellSpursLFQueuePushBody(u64 queue_ea, u64 buffer_ea, u32 is_blocking);
    s32 cellSpursLFQueueDetachLv2EventQueue(u64 queue_ea);
}

// SPURS Fallback for fp=0x1AF3B10ECB1562C3
static void synth2_fallback(spu_context* ctx) {
    printf("[fallback] synth2 SPURS workload running!\n");
    
    // Unblock the PPU waiting for this SPURS task
    cellSpursEventFlagSet((void*)(uintptr_t)0x01178880, 0x0001);
}

extern "C" void ps3_load_prx_modules(void)
{
    // Register SPURS Workload fallback
    spu_workload_register(0x1AF3B10ECB1562C3ULL, synth2_fallback, "synth2");

    // Critical NIDs registered here to guarantee resolution on any build
    ps3_hle_register(0x01220224u, "cellRescGcmSurface2RescSrc", (void*)cellRescGcmSurface2RescSrc);
    ps3_hle_register(0x0D3C22CEu, "cellRescSetWaitFlip", (void*)cellRescSetWaitFlip);
    ps3_hle_register(0xD1CA0503u, "cellRescVideoOutResolutionId2RescBufferMode", (void*)cellRescVideoOutResolutionId2RescBufferMode);
    ps3_hle_register(0x56DFE179u, "cellAudioSetPortLevel", (void*)cellAudioSetPortLevel);
    ps3_hle_register(0x45FE2FCEu, "_sys_spu_printf_initialize", (void*)_sys_spu_printf_initialize);
    ps3_hle_register(0x8A85674Du, "_cellSpursLFQueuePushBody", (void*)_cellSpursLFQueuePushBody);
    ps3_hle_register(0x73E06F91u, "cellSpursLFQueueDetachLv2EventQueue", (void*)cellSpursLFQueueDetachLv2EventQueue);
}
