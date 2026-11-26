// The RomFS overlay lookup (runtime/services/fs.c): every IStorage::Read whose offset is past the game's image end
// asks which appended replacement or added file holds it. The overlay list is sorted by start and the entries are
// laid out back to back in 16-byte units, so the answer is the stored upper bound of `o` by start, one entry back.
//
// This checks that lookup against what the previous linear scan did (exhaustively over small layouts, and by an
// independent oracle on every entry boundary of realistic large ones), and with --bench compares the candidates.
//
// Run: make test-romfs-overlay   /   make bench-romfs-overlay
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "../runtime/services/fs_overlay.h"

// The scan this replaces: first entry whose [start, start + span) holds o.
static int ov_find_linear(const Overlay *ov, int n, uint64_t o) {
    int i = 0;
    while (i < n && !(o >= ov[i].start && o < ov[i].start + ov_span(ov[i].size))) i++;
    return i == n ? -1 : i;
}

// ------------------------------------------------------------------ fixtures
static uint64_t rnd(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *s = x;
}

static void put(Overlay *e, uint64_t start, uint64_t size) {
    e->start = start, e->size = size, e->path = NULL, e->fd = -1, e->mem = NULL;
}

// A layout shaped like a modded boot: many small voice files, a few packs, the extended file table and the RSTB.
// Sizes span the range real replacements have (System/Version.txt to a HUD pack); max_lg caps 2^max_lg. 0-byte
// files are legal (the append loop advances by the rounded size either way).
static uint64_t build_realistic(Overlay *ov, int n, uint64_t base, uint64_t seed, int max_lg) {
    uint64_t at = base;
    for (int i = 0; i < n; i++) {
        uint64_t sz;
        if ((rnd(&seed) & 7) == 0) {
            sz = 0;
        } else {
            int lg = (int)(rnd(&seed) % (uint64_t)(max_lg + 1));
            sz = (1ull << lg) + (lg ? rnd(&seed) & 0xF : 0);
        }
        put(&ov[i], at, sz);
        at += ov_span(sz);
    }
    return at;
}

// Explicitly spaced entries: starts need not be adjacent, which exercises gaps the append loop can't produce but a
// generic lookup must still not claim.
static void build_spaced(Overlay *ov, int n, uint64_t base, uint64_t seed, uint64_t max_gap) {
    uint64_t at = base;
    for (int i = 0; i < n; i++) {
        uint64_t sz = rnd(&seed) % 40;
        put(&ov[i], at, sz);
        at += ov_span(sz) + rnd(&seed) % (max_gap + 1);
    }
}

// Independent oracle: the (unique) entry holding o, without the scan's early exit. More than one match is a broken
// fixture (real layouts never overlap), and is reported.
static int oracle_find(const char *what, const Overlay *ov, int n, uint64_t o) {
    int found = -1, matches = 0;
    for (int i = 0; i < n; i++) {
        if (o >= ov[i].start && o < ov[i].start + ov_span(ov[i].size)) {
            if (!matches) found = i;
            matches++;
        }
    }
    if (matches > 1) {
        fprintf(stderr, "%s: fixture overlap at %#" PRIx64 " (%d entries)\n", what, o, matches);
        exit(1);
    }
    return found;
}

static void fail(const char *what, const char *kind, uint64_t o, int want, int got) {
    fprintf(stderr, "%s: %s at %#" PRIx64 ": want %d, got %d\n", what, kind, o, want, got);
    exit(1);
}

// Every offset in [lo, hi) against a per-offset owner map, so a gap, a zero-size entry or a boundary off by one
// shows up even where the oracle would agree with a similarly wrong answer.
static void check_exhaustive(const char *what, const Overlay *ov, int n, uint64_t lo, uint64_t hi) {
    size_t len = (size_t)(hi - lo);
    int *owner = malloc(len * sizeof *owner);
    if (!owner) {
        fprintf(stderr, "%s: out of memory for %zu offsets\n", what, len);
        exit(1);
    }
    for (size_t k = 0; k < len; k++) owner[k] = -1;
    for (int i = 0; i < n; i++) {
        uint64_t s = ov[i].start, e = s + ov_span(ov[i].size);
        if (s < lo) s = lo;
        if (e > hi) e = hi;
        for (uint64_t o = s; o < e; o++)
            if (owner[o - lo] < 0) owner[o - lo] = i;
    }
    for (size_t k = 0; k < len; k++) {
        uint64_t o = lo + k;
        int want = owner[k], got = ov_find(ov, n, o);
        if (want != got) {
            free(owner);
            fail(what, "lookup", o, want, got);
        }
    }
    free(owner);
}

static void check_sampled(const char *what, const Overlay *ov, int n, uint64_t lo, uint64_t hi, uint64_t seed,
                          int samples) {
    for (int k = 0; k < samples; k++) {
        uint64_t o = lo + rnd(&seed) % (hi - lo);
        int want = oracle_find(what, ov, n, o), got = ov_find(ov, n, o);
        if (want != got) fail(what, "lookup", o, want, got);
    }
}

// Every entry boundary, both sides, and the whole span's first/last byte.
static void check_boundaries(const char *what, const Overlay *ov, int n, uint64_t before) {
    uint64_t probes[6];
    for (int i = 0; i < n; i++) {
        uint64_t s = ov[i].start, e = s + ov_span(ov[i].size);
        probes[0] = s ? s - 1 : 0;
        probes[1] = s;
        probes[2] = ov[i].size ? s + ov[i].size - 1 : s;
        probes[3] = s + ov[i].size;
        probes[4] = e ? e - 1 : 0;
        probes[5] = e;
        for (int k = 0; k < 6; k++) {
            int want = oracle_find(what, ov, n, probes[k]), got = ov_find(ov, n, probes[k]);
            if (want != got) fail(what, "boundary", probes[k], want, got);
        }
    }
    uint64_t extra[] = {0, before ? before - 1 : 0, before, UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - 0xFFF};
    for (size_t k = 0; k < sizeof extra / sizeof *extra; k++) {
        int want = oracle_find(what, ov, n, extra[k]), got = ov_find(ov, n, extra[k]);
        if (want != got) fail(what, "edge", extra[k], want, got);
    }
}

static void check_sorted(const char *what, const Overlay *ov, int n) {
    for (int i = 1; i < n; i++) {
        if (ov[i].start < ov[i - 1].start) {
            fprintf(stderr, "%s: fixture not sorted at %d\n", what, i);
            exit(1);
        }
    }
}

static void test_semantics(void) {
    if (ov_find(NULL, 0, 0) != -1 || ov_find(NULL, 0, UINT64_MAX) != -1) {
        fprintf(stderr, "empty overlay: want -1\n");
        exit(1);
    }
    Overlay one;
    put(&one, 0x1000, 1);  // spans 0x1000..0x100f
    for (uint64_t o = 0xF00; o < 0x1100; o++) {
        int want = o >= 0x1000 && o < 0x1010 ? 0 : -1;
        int got = ov_find(&one, 1, o);
        if (want != got) fail("one file", "lookup", o, want, got);
    }
    if (ov_find(&one, 1, UINT64_MAX) != -1 || ov_find(&one, 1, UINT64_MAX - 0x10) != -1) {
        fprintf(stderr, "one file: huge offset must not match\n");
        exit(1);
    }

    Overlay zero[3];
    put(&zero[0], 0x2000, 0);  // zero-size entries share a start and hold nothing
    put(&zero[1], 0x2000, 0);
    put(&zero[2], 0x2000, 5);
    for (uint64_t o = 0x1FF0; o < 0x2020; o++) {
        int want = o >= 0x2000 && o < 0x2000 + ov_span(5) ? 2 : -1;
        int got = ov_find(zero, 3, o);
        if (want != got) fail("zero-size", "lookup", o, want, got);
    }
}

static void test_exhaustive_layouts(void) {
    static const int sizes[] = {1, 2, 3, 5, 8, 16, 33, 64};
    Overlay ov[512];
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        char what[64];
        snprintf(what, sizeof what, "exhaustive n=%d", sizes[si]);
        uint64_t base = 0x10000;
        uint64_t end = build_realistic(ov, sizes[si], base, seed++, 6);
        check_sorted(what, ov, sizes[si]);
        check_exhaustive(what, ov, sizes[si], base - 0x40, end + 0x40);
        check_boundaries(what, ov, sizes[si], base);
    }
    char what[] = "exhaustive realistic n=512";
    uint64_t base = 0x100000;
    uint64_t end = build_realistic(ov, 512, base, seed++, 6);
    check_sorted(what, ov, 512);
    check_exhaustive(what, ov, 512, base - 0x40, end + 0x40);
    check_boundaries(what, ov, 512, base);
}

static void test_large_layouts(void) {
    enum { N = 400 };
    Overlay ov[N];
    for (int pass = 0; pass < 8; pass++) {
        char what[64];
        snprintf(what, sizeof what, "large pass %d", pass);
        uint64_t seed = 0x123456789ABCDEFull + (uint64_t)pass * 0x9E3779B97F4A7C15ull;
        uint64_t base = 0x10000000;
        uint64_t end = build_realistic(ov, N, base, seed, 22);
        check_sorted(what, ov, N);
        check_boundaries(what, ov, N, base);
        check_sampled(what, ov, N, base, end, seed ^ 0xDEADBEEF, 20000);
        check_sampled(what, ov, N, 0, base, seed ^ 0xC0FFEE, 2000);
    }
}

static void test_spaced_and_adversarial(void) {
    enum { N = 300 };
    Overlay ov[N];
    uint64_t seed = 0xABCDEF01;
    uint64_t base = 0x40000;
    build_spaced(ov, N, base, seed, 0x100);
    check_sorted("spaced", ov, N);
    uint64_t end = ov[N - 1].start + ov_span(ov[N - 1].size);
    check_exhaustive("spaced", ov, N, base - 0x40, end + 0x40);
    check_boundaries("spaced", ov, N, base);

    Overlay one[1];
    put(&one[0], 0x8000, 0);
    check_exhaustive("all zero", one, 1, 0x7F00, 0x8100);

    Overlay same[64];
    for (int i = 0; i < 64; i++) put(&same[i], 0x9000, 0);
    put(&same[63], 0x9000, 1);  // last of the duplicates carries the file
    check_exhaustive("duplicate starts", same, 64, 0x8F00, 0x9100);
}

static void run_tests(void) {
    test_semantics();
    test_exhaustive_layouts();
    test_large_layouts();
    test_spaced_and_adversarial();
    printf("romfs overlay lookup: all tests passed\n");
}

// ------------------------------------------------------------------ benchmarks
#ifdef _WIN32
static uint64_t now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
}
#else
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

// A comparison function pointer, as stdlib bsearch uses: the lowering a direct loop avoids.
static int cmp_start(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// Predecessor search through an indirect comparison (the bsearch cost model). libc bsearch itself only finds exact
// keys, which is not the question a read offset asks, so this is the closest comparable shape.
static int ov_find_fnptr(const Overlay *ov, int n, uint64_t o) {
    if (!n || o < ov[0].start) return -1;
    int lo = 0, hi = n;  // upper bound: first start > o
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (cmp_start(&ov[mid].start, &o) <= 0) lo = mid + 1;
        else hi = mid;
    }
    int i = lo - 1;
    return o < ov[i].start + ov_span(ov[i].size) ? i : -1;
}

// Inlined binary search, branchy. Count-based (std lower_bound shape): only a base pointer and a length.
static int ov_find_branchy(const Overlay *ov, int n, uint64_t o) {
    if (!n || o < ov[0].start) return -1;
    const Overlay *base = ov;
    int len = n;
    while (len > 0) {
        int half = len >> 1;
        if ((base + half)->start <= o) base += half + 1, len -= half + 1;
        else len = half;
    }
    const Overlay *e = base - 1;
    return o < e->start + ov_span(e->size) ? (int)(e - ov) : -1;
}

// The same bounds as the fnptr search, with the comparison written in the loop (the production shape).
static int ov_find_index(const Overlay *ov, int n, uint64_t o) {
    if (!n || o < ov[0].start) return -1;
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (ov[mid].start <= o) lo = mid + 1;
        else hi = mid;
    }
    int i = lo - 1;
    return o < ov[i].start + ov_span(ov[i].size) ? i : -1;
}

// The same, with conditional moves instead of data-dependent branches.
static int ov_find_branchless(const Overlay *ov, int n, uint64_t o) {
    if (!n || o < ov[0].start) return -1;
    const Overlay *base = ov;
    int len = n;
    while (len > 0) {
        int half = len >> 1;
        const Overlay *mid = base + half;
        int go = mid->start <= o;
        base = go ? mid + 1 : base;
        len = go ? len - half - 1 : half;
    }
    const Overlay *e = base - 1;
    return o < e->start + ov_span(e->size) ? (int)(e - ov) : -1;
}

// Eytzinger layout (1-based heap order) of the sorted entries, holding their sorted indices.
static void eytzinger_fill(int *tree, int n, int k, int *next) {
    if (k > n) return;
    eytzinger_fill(tree, n, 2 * k, next);
    tree[k] = (*next)++;
    eytzinger_fill(tree, n, 2 * k + 1, next);
}

static int ov_find_eytzinger(const int *tree, int n, const Overlay *ov, uint64_t o) {
    if (!n || o < ov[0].start) return -1;
    int k = 1, cand = 0;
    while (k <= n) {
        if (ov[tree[k]].start <= o) cand = k, k = 2 * k + 1;
        else k = 2 * k;
    }
    int i = tree[cand];
    return o < ov[i].start + ov_span(ov[i].size) ? i : -1;
}

static uint64_t make_queries(uint64_t *q, size_t nq, const Overlay *ov, int n, uint64_t seed) {
    uint64_t end = ov[0].start;
    for (int i = 0; i < n; i++) {
        uint64_t e = ov[i].start + ov_span(ov[i].size);
        if (e > end) end = e;
    }
    for (size_t k = 0; k < nq; k++) q[k] = ov[0].start + rnd(&seed) % (end - ov[0].start);
    return seed;
}

#define BENCH(label, body)                                                                                \
    do {                                                                                                  \
        uint64_t best = UINT64_MAX;                                                                       \
        uint64_t sink = 0;                                                                                \
        for (int rep = 0; rep < REPS; rep++) {                                                            \
            uint64_t t0 = now_ns();                                                                       \
            for (size_t qi = 0; qi < NQ; qi++) sink += (uint64_t)(int64_t)(body);                         \
            uint64_t dt = now_ns() - t0;                                                                  \
            if (dt < best) best = dt;                                                                     \
        }                                                                                                 \
        double ns = (double)best / (double)NQ;                                                            \
        printf("  %-18s %7.1f ns/lookup  %6.1f Mlookup/s  (checksum %llu)\n", label, ns, 1000.0 / ns,    \
               (unsigned long long)sink);                                                                 \
    } while (0)

static void run_bench(void) {
    static const int ns[] = {16, 64, 256, 512, 1024};
    enum { NQ = 200000, REPS = 5 };
    uint64_t *q = malloc(NQ * sizeof *q);
    Overlay *ov = malloc(1024 * sizeof *ov);
    int *tree = malloc((1024 + 1) * sizeof *tree);
    if (!q || !ov || !tree) {
        fprintf(stderr, "bench: out of memory\n");
        exit(1);
    }
    printf("romfs overlay lookup benchmark (best of %d, %d lookups each, random offsets in the appended region)\n",
           REPS, NQ);
    for (size_t s = 0; s < sizeof ns / sizeof *ns; s++) {
        int n = ns[s];
        uint64_t seed = 0x5EED + (uint64_t)n;
        uint64_t base = 0x10000000;
        build_realistic(ov, n, base, seed, 20);
        seed = make_queries(q, NQ, ov, n, seed);
        int next = 0;
        eytzinger_fill(tree, n, 1, &next);
        printf("n=%d\n", n);
        BENCH("linear (old)", ov_find_linear(ov, n, q[qi]));
        BENCH("binary fnptr", ov_find_fnptr(ov, n, q[qi]));
        BENCH("binary index", ov_find_index(ov, n, q[qi]));
        BENCH("binary branchy", ov_find_branchy(ov, n, q[qi]));
        BENCH("binary branchless", ov_find_branchless(ov, n, q[qi]));
        BENCH("eytzinger", ov_find_eytzinger(tree, n, ov, q[qi]));
        BENCH("under test", ov_find(ov, n, q[qi]));
    }
    free(q), free(ov), free(tree);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        run_bench();
        return 0;
    }
    run_tests();
    return 0;
}
