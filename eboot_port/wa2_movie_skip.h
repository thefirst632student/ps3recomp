#pragma once
#include <cstdlib>
#include <cstring>
#include <cstdio>

// Include after ppu_recomp.h. Only for the verified BLJM60571 executable.
// Workaround: skip playback BEFORE acquiring resources or creating workers.
// This is not a video decoder. Default is OFF.
static inline bool wa2_try_skip_movie(ppu_context* ctx)
{
    const char* mode = std::getenv("WA2_SKIP_MOVIES");
    const bool all = mode && std::strcmp(mode, "1") == 0;
    const bool intro = mode && std::strcmp(mode, "intro") == 0;
    if (!all && !intro) return false;

    // Check the loaded code as well as the patch installer's ELF hash.
    if (ctx->gpr[2] != 0x00149380ULL ||
        vm_read32(0x00082CD0) != 0xF821FE11U ||
        vm_read32(0x00082CD4) != 0x7C0802A6U ||
        vm_read32(0x000833EC) != 0x9864A79CU ||
        vm_read32(0x000833F0) != 0x90653BF4U) {
        std::fprintf(stderr, "[WA2-MOVIE] skip refused: executable/TOC mismatch\n");
        return false;
    }

    // r3 is the relative filename: caller formats "movie/%s.pam".
    const uint64_t path_ea = ctx->gpr[3];
    if (!path_ea || path_ea > 0xFFFFFF00ULL) return false;
    char path[256];
    size_t n = 0;
    for (; n < sizeof(path); ++n) {
        path[n] = static_cast<char>(vm_read8(path_ea + n));
        if (!path[n]) break;
    }
    if (n == sizeof(path) || n < 10 || std::strncmp(path, "movie/", 6) != 0 ||
        std::strcmp(path + n - 4, ".pam") != 0) return false;
    if (intro && std::strcmp(path, "movie/mv00.pam") != 0) return false;

    // Same final flags and success return as 0x833E0..0x833F0.
    // Entry hook runs before the guest stack frame is pushed. Keep LR, SP,
    // TOC and all non-return registers untouched; no worker needs cleanup.
    vm_write8(0x0117A79C, 0);
    vm_write32(0x003F3BF4, 0);
    ctx->gpr[3] = 0;
    std::fprintf(stderr, "[WA2-MOVIE] skipped %s before decoder/thread startup (mode=%s)\n",
                 path, mode);
    return true;
}
