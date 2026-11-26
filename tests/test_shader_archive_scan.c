// The shader archive's magic scan (runtime/nvn/shader_archive.c): bnsh_all and bnsh find their BNSH and grsc magics
// without comparing every 4-byte word. Synthetic buffers only; no game files. `make test-shader-archive` runs the
// equivalence and synthetic-BNSH checks, `make bench-shader-scan` (--bench) runs the previous every-4-bytes scan and
// the memchr-driven one side by side on the same buffers.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../runtime/nvn/shader_archive.c"

// ---- the rest of the archive isn't exercised here (only bnsh_all / bnsh / add_shader are)
int g_stub_binary_calls;
size_t maxwell_binary_size(const uint8_t *code, size_t max_size, int stage) {
    (void)code;
    (void)stage;
    g_stub_binary_calls++;
    return max_size;
}

RomfsImage *romfs_image_open(const char *dir, char *why, size_t why_size) {
    (void)dir;
    if (why && why_size) why[0] = 0;
    return NULL;
}
ssize_t romfs_image_pread(RomfsImage *img, void *dst, uint64_t n, uint64_t off) {
    (void)img;
    (void)dst;
    (void)n;
    (void)off;
    return -1;
}
void romfs_image_close(RomfsImage *img) { (void)img; }

static double now_ns(void) { return (double)clock() * (1e9 / CLOCKS_PER_SEC); }

// ---- the previous scan, kept verbatim as the reference for both checks and the benchmark
static size_t old_positions(const uint8_t *d, size_t n, size_t *pos, size_t cap) {
    size_t c = 0;
    for (size_t o = 0; o + 0x100 <= n; o += 4)
        if (!memcmp(d + o, "BNSH", 4)) {
            if (c < cap) pos[c] = o;
            c++;
        }
    return c;
}

static size_t new_positions(const uint8_t *d, size_t n, size_t *pos, size_t cap) {
    size_t c = 0, end = n >= 0x100 ? n - 0x100 + 1 : 0;
    for (const uint8_t *p = find_magic(d, n, 0, end, "BNSH"); p;) {
        size_t o = (size_t)(p - d);
        if (c < cap) pos[c] = o;
        c++;
        size_t next = o + 4;
        p = next < end ? find_magic(d, n, next, end, "BNSH") : NULL;
    }
    return c;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint8_t rnd8(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint8_t)(rng >> 24);
}

static void check_equivalence(const uint8_t *d, size_t n) {
    size_t a[512], b[512];
    size_t na = old_positions(d, n, a, sizeof a / sizeof *a);
    size_t nb = new_positions(d, n, b, sizeof b / sizeof *b);
    assert(na == nb);
    for (size_t i = 0; i < na && i < sizeof a / sizeof *a; i++) assert(a[i] == b[i]);
}

static void test_scan_equivalence(void) {
    static const size_t sizes[] = {0,  1,  2,  3,  4,  5,  0xFF, 0x100, 0x101, 0x103, 0x104, 0x105,
                                   0x1FF, 0x200, 0x201, 0x400, 0x1000, 0x1003, 4096, 10000};
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        size_t n = sizes[k];
        uint8_t *d = malloc(n ? n : 1);
        assert(d);
        for (size_t i = 0; i < n; i++) d[i] = rnd8();
        check_equivalence(d, n);
        for (int t = 0; t < 8; t++) {  // (magics at random aligned offsets)
            if (n < 0x100) break;
            size_t o = (rnd8() * 4u) % (n - 0x100 + 1);
            o &= ~(size_t)3;
            if (o + 4 <= n) memcpy(d + o, "BNSH", 4);
            check_equivalence(d, n);
        }
        free(d);
    }

    // The last offset the old loop considers is n - 0x100; the byte after it is not scanned.
    uint8_t b[0x108];
    memset(b, 0, sizeof b);
    memcpy(b, "BNSH", 4);
    check_equivalence(b, 0x100);
    memset(b, 0, sizeof b);
    memcpy(b + 4, "BNSH", 4);
    check_equivalence(b, 0x100);
    check_equivalence(b, 0x104);
    check_equivalence(b, 0x103);

    // Non-4-aligned magics are not found, as before; aligned ones are.
    memset(b, 0, sizeof b);
    memcpy(b + 1, "BNSH", 4);
    check_equivalence(b, sizeof b);
    memcpy(b + 3, "BNSH", 4);  // (1 and 3 were not found; 4 is)
    check_equivalence(b, sizeof b);

    // A dense first byte: the fallback to the plain stride must find exactly the same matches.
    static uint8_t dense[0x1100];
    memset(dense, 'B', sizeof dense);
    check_equivalence(dense, sizeof dense);
    memset(dense, 0, sizeof dense);
    for (size_t o = 0; o + 4 <= sizeof dense; o += 4) dense[o] = 'B';
    memcpy(dense + 0x400, "BNSH", 4);
    check_equivalence(dense, sizeof dense);
}

// ---- a synthetic but valid nn::gfx shader file (bnsh: grsc + one variation with a vertex and a fragment stage)
#define BNSH_LEN 0x1800
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static void make_bnsh(uint8_t *b, uint8_t vtag, uint8_t ftag) {
    memset(b, 0, BNSH_LEN);
    memcpy(b, "BNSH", 4);
    memcpy(b + 0x40, "grsc", 4);
    put32(b + 0x40 + 0x1C, 1);     // (one variation)
    put64(b + 0x40 + 0x20, 0x80);  // (variation array)
    put64(b + 0x80 + 0x10, 0x100); // (its program)
    put64(b + 0x100 + 8, 0x140);   // (vertex binary)
    put64(b + 0x100 + 8 + 8 * 4, 0x180); // (fragment binary)
    put64(b + 0x140 + 8, 0x1000);  // (control section, shared)
    put64(b + 0x140 + 0x10, 0x200);
    put32(b + 0x140 + 0x18, 0x100);
    put32(b + 0x140 + 0x1C, 0x760);
    put64(b + 0x180 + 8, 0x1000);
    put64(b + 0x180 + 0x10, 0x400);
    put32(b + 0x180 + 0x18, 0x100);
    put32(b + 0x180 + 0x1C, 0x760);
    put32(b + 0x200, 0x12345678);  // (graphics stages carry the NVN header)
    put32(b + 0x400, 0x12345678);
    b[0x204] = vtag;  // (distinct code, so the two stages hash apart)
    b[0x404] = ftag;
}

static void init_set(SaSet *set) {
    memset(set, 0, sizeof *set);
    set->index = calloc(1, sizeof(Index));
    assert(set->index);
}

static void test_synthetic_bnsh(void) {
    uint8_t block[BNSH_LEN];
    make_bnsh(block, 0x11, 0x22);

    // One file at 0x1000: the scan finds it and both stages become shaders, the pair a program.
    size_t n = 0x2800;
    uint8_t *d = calloc(1, n);
    assert(d);
    memcpy(d + 0x1000, block, BNSH_LEN);
    SaSet set;
    init_set(&set);
    bnsh_all(&set, d, n, 0);
    assert(set.nshaders == 2);
    assert(set.nprograms == 1);
    assert(set.shaders[0].stage == STAGE_VERTEX);
    assert(set.shaders[1].stage == STAGE_FRAGMENT);
    assert(set.programs[0].vs == 0 && set.programs[0].fs == 1);
    shader_archive_free(&set);
    free(d);

    // Two copies at different aligned offsets: both are walked, the identical code is kept once, both programs added.
    n = 0x5000;
    d = calloc(1, n);
    assert(d);
    memcpy(d + 0x1000, block, BNSH_LEN);
    memcpy(d + 0x3000, block, BNSH_LEN);
    init_set(&set);
    bnsh_all(&set, d, n, 0);
    assert(set.nshaders == 2);
    assert(set.nprograms == 2);
    shader_archive_free(&set);
    free(d);

    // Non-4-aligned: the old stride never looked there, and neither does the fix.
    n = 0x3000;
    d = calloc(1, n);
    assert(d);
    memcpy(d + 0x1002, block, BNSH_LEN);
    init_set(&set);
    bnsh_all(&set, d, n, 0);
    assert(set.nshaders == 0 && set.nprograms == 0);
    shader_archive_free(&set);
    free(d);
}

// ---- benchmark: the old stride and the new scan on the same buffer, in the same process
static uint64_t bench_old(const uint8_t *d, size_t n, int iters) {
    uint64_t sink = 0;
    for (int it = 0; it < iters; it++) {
        uint64_t c = 0;
        for (size_t o = 0; o + 0x100 <= n; o += 4)
            if (!memcmp(d + o, "BNSH", 4)) c++;
        sink += c;
    }
    return sink;
}

static uint64_t bench_new(const uint8_t *d, size_t n, int iters) {
    uint64_t sink = 0;
    size_t end = n >= 0x100 ? n - 0x100 + 1 : 0;
    for (int it = 0; it < iters; it++) {
        uint64_t c = 0;
        for (const uint8_t *p = find_magic(d, n, 0, end, "BNSH"); p;) {
            c++;
            size_t next = (size_t)(p - d) + 4;
            p = next < end ? find_magic(d, n, next, end, "BNSH") : NULL;
        }
        sink += c;
    }
    return sink;
}

static void bench_case(const char *name, uint8_t *d, size_t n, int iters) {
    uint64_t a = bench_old(d, n, iters), b = bench_new(d, n, iters);
    assert(a == b);  // (same matches)
    double t0 = now_ns();
    uint64_t ra = bench_old(d, n, iters);
    double old_ms = (now_ns() - t0) / 1e6 / iters;
    t0 = now_ns();
    uint64_t rb = bench_new(d, n, iters);
    double new_ms = (now_ns() - t0) / 1e6 / iters;
    assert(ra == a && rb == b);
    printf("%-16s %10.2f %10.2f %8.1fx  (%llu matches)\n", name, old_ms, new_ms,
           new_ms > 0 ? old_ms / new_ms : 0.0, (unsigned long long)a);
}

static int bench_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x100) {
        fclose(f);
        return 1;
    }
    uint8_t *d = malloc((size_t)sz);
    size_t got = d ? fread(d, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (!d || got != (size_t)sz) {
        free(d);
        return 1;
    }
    size_t nb = 0;
    for (long i = 0; i < sz; i++) nb += d[i] == 'B';
    printf("%s: %ld bytes, 'B' every %.0f bytes\n", path, sz, nb ? (double)sz / (double)nb : 0.0);
    bench_case("file", d, (size_t)sz, 3);
    free(d);
    return 0;
}

static int run_bench(const char *file) {
    const size_t n = (size_t)128 << 20;
    int iters = 3;
    if (file) return bench_file(file);
    uint8_t *d = malloc(n);
    assert(d);
    printf("shader magic scan benchmark: %zu bytes, %d iterations per case\n", n, iters);
    printf("%-16s %10s %10s %9s\n", "case", "old (ms)", "new (ms)", "speedup");

    memset(d, 0, n);
    bench_case("zeros", d, n, iters);

    for (size_t i = 0; i < n; i++) d[i] = rnd8();
    bench_case("random", d, n, iters);

    memset(d, 0, n);
    for (size_t o = 0; o + 0x100 < n; o += 64 * 1024) memcpy(d + (o & ~(size_t)3), "BNSH", 4);
    bench_case("sparse magics", d, n, iters);

    memset(d, 'B', n);
    bench_case("all B (worst)", d, n, 1);

    memset(d, 0, n);
    for (size_t o = 0; o + 0x100 < n; o += 4) d[o] = 'B';
    bench_case("B every word", d, n, 1);

    free(d);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench")) return run_bench(argc > 2 ? argv[2] : NULL);
    test_scan_equivalence();
    test_synthetic_bnsh();
    printf("test_shader_archive_scan: ok\n");
    return 0;
}
