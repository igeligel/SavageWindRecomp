// The per-draw hot loops of runtime/nvn/vulkan/exec.c, measured without a Vulkan device or game data. The functions
// are the ones the renderer calls (exec_hot.h, included by exec.c): the pipeline-key hash, the descriptor-set
// content hash, the index scan / expansion and the vertex-stream range. Sizes are the open world's: a few thousand
// draws a frame, 1-6k indices, two descriptor sets a draw and up to 17 vertex streams.
//
// `make test-vk-exec-hot` checks the answers; `make bench-vk-exec-hot` prints ns/op for each path. The "per draw"
// lines repeat the median case, so a result can be compared with the time a 5000-draw frame spends in that loop.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../runtime/nvn/vulkan/exec_hot.h"

static volatile uint64_t sink;

static double now_ns(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

typedef void (*bench_fn)(const void *);

static double bench(const char *name, bench_fn fn, const void *ctx, int batch, const char *data) {
    for (int i = 0; i < 10; i++) fn(ctx);  // warm the branch predictors and caches
    double t0 = now_ns();
    long reps = 0;
    do {
        fn(ctx);
        reps++;
    } while (now_ns() - t0 < 50e6 && reps < 1000000);  // ~50 ms of work
    double ns = (now_ns() - t0) / ((double)reps * batch);
    printf("  %-34s %-18s %9.2f ns   %7.2f M/s\n", name, data, ns, ns > 0 ? 1000.0 / ns : 0.0);
    return ns;
}

// ---- index scan (the copy bounds a vertex stream: largest index, primitive restart ignored)
typedef struct {
    const void *indices;
    uint32_t count, restart, max_index;
} IdxCase;

static void run_idx_u16(const void *p) {
    const IdxCase *c = p;
    sink += idx_max_u16(c->indices, c->count, c->restart, c->max_index);
}

static void run_idx_u32(const void *p) {
    const IdxCase *c = p;
    sink += idx_max_u32(c->indices, c->count, c->restart, c->max_index);
}

// The pre-change loops, kept here to show the A/B in bench-vk-exec-hot.
static uint32_t idx_max_u16_scalar(const uint16_t *s, uint32_t count, uint32_t restart, uint32_t max_index) {
    uint16_t r = (uint16_t)restart;
    for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != r) max_index = s[i];
    return max_index;
}

static uint32_t idx_max_u32_scalar(const uint32_t *s, uint32_t count, uint32_t restart, uint32_t max_index) {
    for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != restart) max_index = s[i];
    return max_index;
}

static void run_idx_u16_scalar(const void *p) {
    const IdxCase *c = p;
    sink += idx_max_u16_scalar(c->indices, c->count, c->restart, c->max_index);
}

static void run_idx_u32_scalar(const void *p) {
    const IdxCase *c = p;
    sink += idx_max_u32_scalar(c->indices, c->count, c->restart, c->max_index);
}

// ---- index expansion (strip / fan / quad to a triangle list). old: malloc + copy to the ring; new: fill the ring.
typedef struct {
    const void *indices;
    uint32_t mode, count;
    uint32_t *out;
} ExpandCase;

static void run_expand_old(const void *p) {
    const ExpandCase *c = p;
    uint32_t n;
    uint32_t *e = expand_indices(c->mode, c->count, c->indices, 1, 0, &n);
    memcpy(c->out, e, (size_t)n * 4);
    free(e);
    sink += c->out[0] + c->out[n / 2] + c->out[n - 1];
}

static void run_expand_into(const void *p) {
    const ExpandCase *c = p;
    uint32_t n = expand_indices_count(c->mode, c->count);
    expand_indices_into(c->out, c->mode, c->count, c->indices, 1, 0);
    sink += c->out[0] + c->out[n / 2] + c->out[n - 1];
}

// ---- vertex range (per used stream, the largest index / instance bounds the copy)
#define BENCH_STREAMS 17
typedef struct {
    uint32_t max_index, inst, base_inst, nstream;
    uint64_t stride[BENCH_STREAMS], size[BENCH_STREAMS];
    uint32_t divisor[BENCH_STREAMS];
} VertexCase;

static void run_vertex(const void *p) {
    const VertexCase *c = p;
    uint64_t total = 0;
    for (int b = 0; b < 1000; b++) {
        uint32_t max_index = c->max_index + (b & 1);  // (kept in the loop: no constant folding of the range)
        for (uint32_t s = 0; s < c->nstream; s++)
            total += vertex_need(max_index, c->inst, c->base_inst, c->stride[s], c->divisor[s], c->size[s]);
    }
    sink += total;
}

// ---- descriptor set content hash (set_finish's two-hash key)
#define BENCH_BINDS 24
typedef struct {
    int n;
    uint64_t v[BENCH_BINDS][4];
} SetCase;

static void run_set(const void *p) {
    const SetCase *c = p;
    uint64_t h = 0x123456789abcdefull, h2 = h ^ 0x6A09E667F3BCC909ull;
    for (int b = 0; b < 100; b++)
        for (int i = 0; i < c->n; i++) set_hash_add(&h, &h2, c->v[i]);
    sink += h + h2;
}

// ---- pipeline-key hash and the memo compare (PipeKey is ~688 bytes on x86-64/aarch64)
typedef struct {
    const void *key, *other;
    size_t size;
} KeyCase;

static void run_key_hash(const void *p) {
    const KeyCase *c = p;
    sink += hash_words(0xcbf29ce484222325ull, c->key, c->size);
}

static void run_key_memcmp(const void *p) {
    const KeyCase *c = p;
    sink += (uint64_t)memcmp(c->key, c->other, c->size);
}

static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test(void) {
    // index scans ignore the restart sentinel and keep the largest index
    uint16_t i16[8] = {0, 4, 0xFFFF, 7, 2, 9, 0xFFFF, 1};
    uint32_t i32[8] = {0, 4, 0xFFFFFFFFu, 7, 2, 9, 0xFFFFFFFFu, 1};
    assert(idx_max_u16(i16, 8, 0xFFFF, 0) == 9);
    assert(idx_max_u32(i32, 8, 0xFFFFFFFFu, 0) == 9);
    assert(idx_max_u16(i16, 8, 0xFFFF, 12) == 12);

    // the unrolled scans match the pre-change loops for every count and restart value, including the fast paths
    static uint16_t r16[257];
    static uint32_t r32[257];
    for (int i = 0; i < 257; i++) {
        r16[i] = (uint16_t)rng();
        r32[i] = rng();
    }
    const uint32_t restarts[] = {0, 5, 0xFFFF, 0xFFFFFFFFu};
    for (unsigned k = 0; k < sizeof restarts / sizeof restarts[0]; k++) {
        for (uint32_t n = 0; n <= 257; n++) {
            assert(idx_max_u16(r16, n, restarts[k], 0) == idx_max_u16_scalar(r16, n, restarts[k], 0));
            assert(idx_max_u32(r32, n, restarts[k], 0) == idx_max_u32_scalar(r32, n, restarts[k], 0));
            assert(idx_max_u16(r16, n, restarts[k], 1234) == idx_max_u16_scalar(r16, n, restarts[k], 1234));
            assert(idx_max_u32(r32, n, restarts[k], 1234) == idx_max_u32_scalar(r32, n, restarts[k], 1234));
        }
    }

    // vertex ranges: constant, indexed, instanced and clamped to the guest range
    assert(vertex_need(100, 1, 0, 0, 0, 0) == 256);
    assert(vertex_need(100, 1, 0, 32, 0, 0) == 100 * 32 + 256);
    assert(vertex_need(100, 3, 10, 32, 2, 0) == (10 + 3 + 1) / 2 * 32 + 256);
    assert(vertex_need(100, 1, 0, 32, 0, 1024) == 1024);

    // expansion: a fan and two quads become the same triangles the renderer draws
    uint16_t fan[5] = {0, 1, 2, 3, 4};
    uint16_t quad[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint32_t n;
    uint32_t *e = expand_indices(6, 5, fan, 1, 0, &n);
    assert(n == 9 && e[0] == 0 && e[1] == 1 && e[2] == 2 && e[6] == 0 && e[7] == 3 && e[8] == 4);
    free(e);
    e = expand_indices(7, 8, quad, 1, 0, &n);
    assert(n == 12 && e[0] == 0 && e[1] == 1 && e[2] == 2 && e[3] == 0 && e[4] == 2 && e[5] == 3 &&
           e[6] == 4 && e[7] == 5 && e[8] == 6 && e[9] == 4 && e[10] == 6 && e[11] == 7);
    free(e);

    // the in-place form produces the same bytes and counts as the malloc form
    uint32_t ring[32];
    assert(expand_indices_count(6, 5) == 9 && expand_indices_count(7, 8) == 12 && expand_indices_count(4, 2) == 0);
    expand_indices_into(ring, 6, 5, fan, 1, 0);
    assert(ring[0] == 0 && ring[1] == 1 && ring[2] == 2 && ring[6] == 0 && ring[7] == 3 && ring[8] == 4);
    expand_indices_into(ring, 7, 8, quad, 1, 0);
    assert(ring[0] == 0 && ring[1] == 1 && ring[2] == 2 && ring[3] == 0 && ring[4] == 2 && ring[5] == 3 &&
           ring[6] == 4 && ring[7] == 5 && ring[8] == 6 && ring[9] == 4 && ring[10] == 6 && ring[11] == 7);

    // hashes are stable and distinguish a changed byte
    uint8_t a[64] = {0}, b[64] = {0};
    b[63] = 1;
    assert(hash_words(0, a, sizeof a) == hash_words(0, a, sizeof a));
    assert(hash_words(0, a, sizeof a) != hash_words(0, b, sizeof b));
    uint64_t h0 = 1, h1 = 2, k0 = 1, k1 = 2;
    uint64_t v0[4] = {1, 2, 3, 4}, v1[4] = {1, 2, 3, 5};
    set_hash_add(&h0, &h1, v0);
    set_hash_add(&k0, &k1, v0);
    assert(h0 == k0 && h1 == k1);
    set_hash_add(&k0, &k1, v1);
    assert(h0 != k0 && h1 != k1);

    puts("PASS: exec hot paths (hash, sets, index scan/expand, vertex range)");
}

int main(int argc, char **argv) {
    test();
    if (argc < 2 || strcmp(argv[1], "--bench")) return 0;

    // One open-world frame's rough shape: ~5000 draws, 2 sets a draw, 3000 indexed, 1-6k indices, a few streams.
    static uint16_t i16[6144];
    static uint32_t i32[6144];
    for (int i = 0; i < 6144; i++) {
        uint32_t v = rng() % 4096;
        i16[i] = (i % 97 == 0) ? 0xFFFF : (uint16_t)v;
        i32[i] = (i % 97 == 0) ? 0xFFFFFFFFu : v;
    }

    puts("draw/key (pipeline key, ~688 bytes)");
    static uint8_t key[688], key2[688];
    for (size_t i = 0; i < sizeof key; i++) key[i] = key2[i] = (uint8_t)rng();
    KeyCase kc = {key, key, sizeof key};
    KeyCase kc2 = {key2, key2, sizeof key2};
    double key_hash = bench("pipeline-key hash", run_key_hash, &kc, 1, "688 B");
    double key_cmp = bench("memo memcmp", run_key_memcmp, &kc2, 1, "688 B");
    printf("  per draw: %.1f ns hash (only when the key changed) + %.1f ns memo memcmp\n", key_hash, key_cmp);

    puts("sets (descriptor-set content hash, vertex + fragment)");
    static SetCase vset, fset;
    vset.n = 4;
    for (int i = 0; i < vset.n; i++) {
        vset.v[i][0] = (uint32_t)i | (uint64_t)6 << 32;  // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
        vset.v[i][1] = 0x100000000ull + (uint64_t)i * 0x1000;
        vset.v[i][2] = (uint64_t)i * 256;
        vset.v[i][3] = 256;
    }
    fset.n = 20;  // 16 textures + 4 constant buffers, as a material draw commonly binds
    for (int i = 0; i < fset.n; i++) {
        if (i < 16) {
            fset.v[i][0] = (uint32_t)(32 + i) | (uint64_t)1 << 32;  // COMBINED_IMAGE_SAMPLER
            fset.v[i][1] = 0x200000000ull + (uint64_t)i * 0x40;
            fset.v[i][2] = 0x300000000ull + (uint64_t)i * 0x4000;
            fset.v[i][3] = 5;
        } else {
            fset.v[i][0] = (uint32_t)(i - 16) | (uint64_t)6 << 32;
            fset.v[i][1] = 0x100000000ull + (uint64_t)i * 0x1000;
            fset.v[i][2] = 0;
            fset.v[i][3] = 512;
        }
    }
    double set_v = bench("vertex set, 4 UBOs", run_set, &vset, 100, "4 bindings");
    double set_f = bench("fragment set, 16 tex + 4 UBO", run_set, &fset, 100, "20 bindings");
    printf("  per draw: %.1f ns for both sets\n", set_v + set_f);

    puts("vtx (index scan: pre-change scalar vs unrolled branchless)");
    static const uint32_t sizes[] = {256, 1536, 6144};
    double old16[3], new16[3], old32[3], new32[3];
    for (int i = 0; i < 3; i++) {
        char label[32];
        IdxCase c = {i16, sizes[i], 0xFFFF, 0};
        snprintf(label, sizeof label, "u16 old, %u indices", sizes[i]);
        old16[i] = bench("index scan", run_idx_u16_scalar, &c, 1, label);
        snprintf(label, sizeof label, "u16 new, %u indices", sizes[i]);
        new16[i] = bench("index scan", run_idx_u16, &c, 1, label);
    }
    for (int i = 0; i < 3; i++) {
        char label[32];
        IdxCase c = {i32, sizes[i], 0xFFFFFFFFu, 0};
        snprintf(label, sizeof label, "u32 old, %u indices", sizes[i]);
        old32[i] = bench("index scan", run_idx_u32_scalar, &c, 1, label);
        snprintf(label, sizeof label, "u32 new, %u indices", sizes[i]);
        new32[i] = bench("index scan", run_idx_u32, &c, 1, label);
    }
    printf("  per draw at 1536: u16 %.1f -> %.1f ns (%.2fx), u32 %.1f -> %.1f ns (%.2fx)\n",
           old16[1], new16[1], old16[1] / new16[1], old32[1], new32[1], old32[1] / new32[1]);

    puts("vtx (index expansion: malloc + copy vs fill the transient ring)");
    static uint32_t ring[20000];
    for (int i = 1; i < 3; i++) {
        char label[32];
        ExpandCase c = {i16, 6, sizes[i], ring};
        snprintf(label, sizeof label, "fan old, %u indices", sizes[i]);
        double fo = bench("expand to triangles", run_expand_old, &c, 1, label);
        snprintf(label, sizeof label, "fan new, %u indices", sizes[i]);
        double fn = bench("expand to triangles", run_expand_into, &c, 1, label);
        if (i == 1) printf("  per fan draw: %.1f -> %.1f ns (%.2fx)\n", fo, fn, fo / fn);
        c.mode = 7;
        snprintf(label, sizeof label, "quads old, %u indices", sizes[i]);
        double qo = bench("expand to triangles", run_expand_old, &c, 1, label);
        snprintf(label, sizeof label, "quads new, %u indices", sizes[i]);
        double qn = bench("expand to triangles", run_expand_into, &c, 1, label);
        if (i == 1) printf("  per quad draw: %.1f -> %.1f ns (%.2fx)\n", qo, qn, qo / qn);
    }

    puts("vtx (vertex-stream range, per used stream)");
    VertexCase vc = {.max_index = 1536, .inst = 1, .base_inst = 0};
    for (int i = 0; i < 4; i++) {
        uint64_t s = (uint64_t[]){12, 16, 32, 48}[i];
        vc.stride[i] = s;
        vc.divisor[i] = i == 3 ? 1 : 0;  // one instanced stream
        vc.size[i] = 0;
    }
    for (int n = 1; n <= 4; n++) {
        char label[32];
        vc.nstream = n;
        snprintf(label, sizeof label, "%d stream%s", n, n == 1 ? "" : "s");
        double ns = bench("vertex range", run_vertex, &vc, 1000, label);
        if (n == 4) printf("  per draw: %.1f ns for 4 streams (%.2f ns a stream)\n", ns, ns / 4);
    }
    return 0;
}
