// Quads / fans / polygons as a 32-bit triangle index list. exec.c fills the draw's transient index span in place;
// expand_indices is the malloc-based form for callers without a span (and for tests).
#ifndef SW_EXEC_HELPERS_INDEX_EXPAND_H
#define SW_EXEC_HELPERS_INDEX_EXPAND_H

#include <stdint.h>
#include <stdlib.h>

// How many 32-bit indices an expansion produces (the transient buffer is allocated with this before it is filled).
static inline uint32_t expand_indices_count(uint32_t mode, uint32_t count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    return tris * 3;
}

static inline void expand_indices_into(uint32_t *out, uint32_t mode, uint32_t count, const void *indices, int itype,
                                       uint32_t first) {
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
}

static inline uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first,
                                       uint32_t *out_count) {
    uint32_t n = expand_indices_count(mode, count);
    uint32_t *out = malloc(n * 4 + 4);
    expand_indices_into(out, mode, count, indices, itype, first);
    *out_count = n;
    return out;
}

#endif
