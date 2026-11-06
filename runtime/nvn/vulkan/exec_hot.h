// The per-draw loops of exec.c that are worth timing on their own: the pipeline-key hash, the descriptor-set content
// hash, the index scan / expansion and the vertex-stream range. They live here so tests/test_vk_exec_hot.c measures
// the same code the renderer runs, without a Vulkan device or game data. Keep them unchanged in behaviour; the tests
// assert the results and bench-* reports the cost.
#ifndef SW_EXEC_HOT_H
#define SW_EXEC_HOT_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static inline uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

// One descriptor of set_finish's two-hash content key: binding | type, then sampler/view/layout (image) or
// buffer/offset/range (buffer).
static inline void set_hash_add(uint64_t *h, uint64_t *h2, const uint64_t v[4]) {
    for (int k = 0; k < 4; k++) {
        *h = (*h ^ v[k]) * 0x9E3779B97F4A7C15ull; *h ^= *h >> 29;
        *h2 = (*h2 ^ v[k]) * 0xC2B2AE3D27D4EB4Full; *h2 ^= *h2 >> 31;
    }
}

// The largest index a draw can read, ignoring the primitive-restart sentinel (the index scan that bounds a vertex
// stream copy). Four independent maxima, eight elements a step: the branchless select and the unrolled chains let the
// compiler use cmov/vector min-max instead of one compare-and-branch per index (a 1536-index draw scanned in ~0.9 us
// before). Mapping the sentinel to 0 is exact: every index is unsigned and the running maximum starts at 0.
static inline uint32_t idx_max_u16(const uint16_t *s, uint32_t count, uint32_t restart, uint32_t max_index) {
    uint16_t r = (uint16_t)restart;
    uint32_t m0 = max_index, m1 = max_index, m2 = max_index, m3 = max_index;
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        uint16_t a = s[i + 0], b = s[i + 1], c = s[i + 2], d = s[i + 3];
        uint16_t e = s[i + 4], f = s[i + 5], g = s[i + 6], h = s[i + 7];
        a = a == r ? 0 : a; b = b == r ? 0 : b; c = c == r ? 0 : c; d = d == r ? 0 : d;
        e = e == r ? 0 : e; f = f == r ? 0 : f; g = g == r ? 0 : g; h = h == r ? 0 : h;
        m0 = a > m0 ? a : m0; m1 = b > m1 ? b : m1; m2 = c > m2 ? c : m2; m3 = d > m3 ? d : m3;
        m0 = e > m0 ? e : m0; m1 = f > m1 ? f : m1; m2 = g > m2 ? g : m2; m3 = h > m3 ? h : m3;
    }
    for (; i < count; i++) {
        uint16_t v = s[i];
        v = v == r ? 0 : v;
        m0 = v > m0 ? v : m0;
    }
    m0 = m1 > m0 ? m1 : m0;
    m2 = m3 > m2 ? m3 : m2;
    return m0 > m2 ? m0 : m2;
}

static inline uint32_t idx_max_u32(const uint32_t *s, uint32_t count, uint32_t restart, uint32_t max_index) {
    uint32_t m0 = max_index, m1 = max_index, m2 = max_index, m3 = max_index;
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        uint32_t a = s[i + 0], b = s[i + 1], c = s[i + 2], d = s[i + 3];
        uint32_t e = s[i + 4], f = s[i + 5], g = s[i + 6], h = s[i + 7];
        a = a == restart ? 0 : a; b = b == restart ? 0 : b;
        c = c == restart ? 0 : c; d = d == restart ? 0 : d;
        e = e == restart ? 0 : e; f = f == restart ? 0 : f;
        g = g == restart ? 0 : g; h = h == restart ? 0 : h;
        m0 = a > m0 ? a : m0; m1 = b > m1 ? b : m1; m2 = c > m2 ? c : m2; m3 = d > m3 ? d : m3;
        m0 = e > m0 ? e : m0; m1 = f > m1 ? f : m1; m2 = g > m2 ? g : m2; m3 = h > m3 ? h : m3;
    }
    for (; i < count; i++) {
        uint32_t v = s[i];
        v = v == restart ? 0 : v;
        m0 = v > m0 ? v : m0;
    }
    m0 = m1 > m0 ? m1 : m0;
    m2 = m3 > m2 ? m3 : m2;
    return m0 > m2 ? m0 : m2;
}

// How many bytes of a vertex stream this draw can read. stride 0: the constant zero stream; divisor: instanced
// stream; otherwise the largest index the draw reads. A known guest range bounds it.
static inline uint64_t vertex_need(uint32_t max_index, uint32_t inst, uint32_t base_inst, uint64_t stride,
                                   uint32_t divisor, uint64_t size) {
    uint64_t need;
    if (!stride) need = 256;
    else if (divisor) need = ((uint64_t)base_inst + inst + divisor - 1) / divisor * stride + 256;
    else need = (uint64_t)max_index * stride + 256;
    if (size && need > size) need = size;
    return need;
}

// How many 32-bit indices an expansion produces (the transient buffer is allocated with this before it is filled).
static inline uint32_t expand_indices_count(uint32_t mode, uint32_t count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    return tris * 3;
}

// Quads / fans / polygons as a 32-bit triangle index list. out is the draw's transient index buffer, so the renderer
// fills it in place instead of expanding into malloc and copying it to the ring afterwards.
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

// malloc-based form, for tests and callers without a transient span.
static inline uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first,
                                       uint32_t *out_count) {
    uint32_t n = expand_indices_count(mode, count);
    uint32_t *out = malloc(n * 4 + 4);
    expand_indices_into(out, mode, count, indices, itype, first);
    *out_count = n;
    return out;
}

#endif
