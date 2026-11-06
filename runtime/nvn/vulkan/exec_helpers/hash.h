// The hash exec.c uses for pipeline keys and other in-memory tables. Kept in a header so tests can measure the exact
// loop the renderer runs. hmix values are compared with stored constants: changing an algorithm changes table keys.
#ifndef SW_EXEC_HELPERS_HASH_H
#define SW_EXEC_HELPERS_HASH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw).
static inline uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

#endif
