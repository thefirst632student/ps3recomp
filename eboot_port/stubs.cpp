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
}
