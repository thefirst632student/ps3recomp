#ifndef EDGE_LZMA_H
#define EDGE_LZMA_H

#include <stdint.h>
#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decompresses or copies an Edge LZMA LFQueue element at guest EA buffer_ea.
 * Handles countdown workToDoCounter and signaling cellSpursEventFlag upon completion.
 */
s32 edge_lzma_process_element(uint64_t buffer_ea);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_LZMA_H */
