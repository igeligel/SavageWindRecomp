// The content key of a descriptor set (exec.c set_finish): two independent 64-bit hashes over each binding, so
// identical sets share one descriptor set without storing and comparing the full writes.
#ifndef SW_EXEC_HELPERS_SET_HASH_H
#define SW_EXEC_HELPERS_SET_HASH_H

#include <stdint.h>

// binding | type, then sampler/view/layout (image) or buffer/offset/range (buffer).
static inline void set_hash_add(uint64_t *h, uint64_t *h2, const uint64_t v[4]) {
    for (int k = 0; k < 4; k++) {
        *h = (*h ^ v[k]) * 0x9E3779B97F4A7C15ull; *h ^= *h >> 29;
        *h2 = (*h2 ^ v[k]) * 0xC2B2AE3D27D4EB4Full; *h2 ^= *h2 >> 31;
    }
}

#endif
