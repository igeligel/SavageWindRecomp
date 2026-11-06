// The largest index a draw can read, ignoring the primitive-restart sentinel. exec.c uses it to bound the vertex
// stream copy.
#ifndef SW_EXEC_HELPERS_INDEX_SCAN_H
#define SW_EXEC_HELPERS_INDEX_SCAN_H

#include <stdint.h>

// Four independent maxima, eight elements a step: the branchless select and the unrolled chains let the compiler use
// cmov/vector min-max instead of one compare-and-branch per index (a 1536-index draw scanned in ~0.9 us before).
// Mapping the sentinel to 0 is exact: every index is unsigned and the running maximum starts at 0.
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

#endif
