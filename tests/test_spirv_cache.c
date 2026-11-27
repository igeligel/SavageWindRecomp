// The SPIR-V cache key and the ensure/compile handoff (runtime/nvn/vulkan/shaders/spirv_*.c) without a Vulkan device,
// shaderc or game data: a fake compiler produces words, a memory cache stands in for cache/spirv, and a synthetic
// "translated" GLSL source stands in for a shader. It pins the hash bytes (the name of every stored module), the
// disk file's edges (bad magic, wrong/zero/huge word counts, short and non-deflate payloads) and the handoff that
// hashes the source once. `--bench` times the key on shader-sized sources. Runtime tests: Makefile test-spirv.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "nvn/maxwell/shader.h"
#include "nvn/vulkan/shaders/cache_path.h"
#include "nvn/vulkan/shaders/spirv_cache.h"
#include "nvn/vulkan/shaders/spirv_core.h"
#include "nvn/vulkan/shaders/spirv_hash.h"

#define WORD(op, wc) (((uint32_t)(wc) << 16) | (uint32_t)(op))
#define OP_NAME 5u
#define OP_SOURCE 3u
#define OP_DECORATE 71u
#define OP_TYPE_VOID 19u
#define OP_FADD 129u
#define OP_FNEGATE 127u
#define OP_EXT_INST 12u
#define DEC_NO_CONTRACTION 42u

static void env_set(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    setenv(k, v, 1);
#endif
}
static void env_unset(const char *k) {
#ifdef _WIN32
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}

static uint32_t *dup_words(const uint32_t *w, size_t n) {
    uint32_t *d = malloc(n ? n * 4 : 4);
    assert(d);
    if (n) memcpy(d, w, n * 4);
    return d;
}

// -------------------------------------------------------------------------------------- key: exact bytes
// The pre-split vk_glsl_to_spirv/src_hash formula. Every stored module is named by this digest: the refactor must not
// change one byte of it.
static uint64_t reference_hash(const char *id, const char *src, int stage, int opt, int api12) {
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)stage ^ (opt ? 0x6f707400ull : 0) ^ (api12 ? 0x31320000ull : 0);
    for (const char *p = id; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

static void test_hash(void) {
    const char *id = "0123456789abcdef";
    static const char *const srcs[] = {
        "",
        "void main(){}",
        "#version 460\nlayout(std140, set=0, binding=0) uniform CB0 { uvec4 cb0[4096]; };\nvoid main(){}\n",
        ("\x80\x81\xff\xfe" " high bytes (uint8_t, not char) and a NUL after this"),
    };
    static const int stages[] = {STAGE_VERTEX, STAGE_FRAGMENT, STAGE_COMPUTE, 7};
    spirv_hash_set_id(id);
    spirv_hash_set_impl(NULL);
    for (size_t s = 0; s < sizeof srcs / sizeof srcs[0]; s++)
        for (size_t st = 0; st < sizeof stages / sizeof stages[0]; st++)
            for (int opt = 0; opt <= 1; opt++)
                for (int api = 0; api <= 1; api++) {
                    size_t len = (size_t)-1;
                    uint64_t h = spirv_src_hash(srcs[s], stages[st], opt, api, &len);
                    assert(len == strlen(srcs[s]));
                    assert(h == reference_hash(id, srcs[s], stages[st], opt, api));
                }
    // each switch changes the key, and the id's seed is recomputed (not kept from the previous id)
    assert(spirv_src_hash("a", STAGE_VERTEX, 0, 0, NULL) != spirv_src_hash("a", STAGE_FRAGMENT, 0, 0, NULL));
    assert(spirv_src_hash("a", STAGE_VERTEX, 0, 0, NULL) != spirv_src_hash("a", STAGE_VERTEX, 1, 0, NULL));
    assert(spirv_src_hash("a", STAGE_VERTEX, 0, 0, NULL) != spirv_src_hash("a", STAGE_VERTEX, 0, 1, NULL));
    spirv_hash_set_id("fedcba9876543210");
    assert(spirv_src_hash("a", STAGE_VERTEX, 1, 1, NULL) == reference_hash("fedcba9876543210", "a", STAGE_VERTEX, 1, 1));
    spirv_hash_set_id(id);
    assert(spirv_src_hash("a", STAGE_VERTEX, 1, 1, NULL) == reference_hash(id, "a", STAGE_VERTEX, 1, 1));
    puts("ok  key: bytes, length, stage/opt/api12/id");
}

// -------------------------------------------------------------------------------------- strip_names
static void test_strip_names(void) {
    uint32_t mod[] = {
        0x07230203, 0x00010000, 0, 16, 0,
        WORD(OP_NAME, 3), 7, 0x00690068,     // OpName 7 "hi"
        WORD(OP_SOURCE, 3), 0, 0x00010000,   // OpSource
        WORD(OP_TYPE_VOID, 2), 7,            // kept
        WORD(OP_TYPE_VOID, 2), 8,            // kept
    };
    size_t n = sizeof mod / sizeof mod[0];
    spirv_strip_names(mod, &n);
    assert(n == 9);
    assert(mod[5] == WORD(OP_TYPE_VOID, 2) && mod[6] == 7);
    assert(mod[7] == WORD(OP_TYPE_VOID, 2) && mod[8] == 8);

    // a zero word count and a truncated last instruction leave the module as it was
    uint32_t zero[] = {0x07230203, 0x00010000, 0, 8, 0, WORD(OP_TYPE_VOID, 0), 0};
    size_t zn = sizeof zero / sizeof zero[0];
    uint32_t zero_copy[sizeof zero / sizeof zero[0]];
    memcpy(zero_copy, zero, sizeof zero);
    spirv_strip_names(zero, &zn);
    assert(zn == sizeof zero / sizeof zero[0] && !memcmp(zero, zero_copy, sizeof zero));

    uint32_t trunc[] = {0x07230203, 0x00010000, 0, 8, 0, WORD(OP_TYPE_VOID, 2)};
    size_t tn = sizeof trunc / sizeof trunc[0];
    spirv_strip_names(trunc, &tn);
    assert(tn == sizeof trunc / sizeof trunc[0]);
    puts("ok  strip_names: names removed, malformed left alone");
}

// -------------------------------------------------------------------------------------- no_contraction
static void test_no_contraction(void) {
    const uint32_t bound = 64;
    static const uint32_t template[] = {
        0x07230203, 0x00010000, 0, bound, 0,
        WORD(OP_DECORATE, 3), 7, DEC_NO_CONTRACTION,       // already decorated
        WORD(OP_TYPE_VOID, 2), 1,                          // first type decl: insertion point
        WORD(OP_FADD, 5), 1, 7, 3, 4,                      // id 7: already done
        WORD(OP_FADD, 5), 1, 20, 3, 4,                     // add
        WORD(OP_EXT_INST, 5), 1, 21, 0, 50,                // Fma: add
        WORD(OP_FNEGATE, 4), 1, 22, 3,                     // add
        WORD(OP_EXT_INST, 5), 1, 23, 0, 51,                // not Fma: skip
        WORD(OP_DECORATE, 3), 30, DEC_NO_CONTRACTION,      // another id's decoration
        WORD(OP_FADD, 5), 1, 64, 3, 4,                     // id == bound: skip
    };
    size_t orig = sizeof template / sizeof template[0], n = orig;
    uint32_t *mod = dup_words(template, orig);  // (spirv_no_contraction frees its input: as in production)
    uint32_t *out = spirv_no_contraction(mod, &n);
    assert(out != mod && n == orig + 3 * 3);  // ids 20, 21, 22
    assert(out[5] == WORD(OP_DECORATE, 3) && out[6] == 7 && out[7] == DEC_NO_CONTRACTION);  // original kept first
    size_t deco[4] = {0, 0, 0, 0};
    for (size_t o = 5; o < n;) {
        uint32_t wc = out[o] >> 16, op = out[o] & 0xFFFF;
        if (op == OP_DECORATE && wc == 3 && out[o + 2] == DEC_NO_CONTRACTION) {
            if (out[o + 1] == 7) deco[0]++;
            if (out[o + 1] == 20) deco[1]++;
            if (out[o + 1] == 21) deco[2]++;
            if (out[o + 1] == 22) deco[3]++;
            assert(out[o + 1] != 23 && out[o + 1] != 64);  // not Fma, and id == bound
        }
        o += wc;
    }
    assert(deco[0] == 1 && deco[1] == 1 && deco[2] == 1 && deco[3] == 1);  // no duplicate for 7; one each for 20/21/22
    free(out);

    // nothing to add: the same module comes back
    uint32_t none[] = {0x07230203, 0x00010000, 0, 8, 0, WORD(OP_TYPE_VOID, 2), 1, WORD(OP_TYPE_VOID, 2), 2};
    size_t nn = sizeof none / sizeof none[0];
    uint32_t *same = spirv_no_contraction(none, &nn);
    assert(same == none && nn == sizeof none / sizeof none[0]);
    puts("ok  no_contraction: add once, skip done/bound/unrelated");
}

// -------------------------------------------------------------------------------------- a swappable compiler + cache
static int compiler_calls, fail_compile;
static uint32_t fake_words[64];
static size_t fake_nwords;

static void set_fake(const uint32_t *w, size_t n) {
    assert(n <= sizeof fake_words / sizeof fake_words[0]);
    memcpy(fake_words, w, n * 4);
    fake_nwords = n;
}

static uint32_t *fake_compile(const char *src, size_t len, int stage, int optimize, int api12, const char *name,
                              size_t *nwords) {
    (void)optimize, (void)api12, (void)name;
    compiler_calls++;
    assert(len == strlen(src));  // the length came from the key, not a second scan
    assert(stage == STAGE_VERTEX || stage == STAGE_FRAGMENT || stage == STAGE_COMPUTE);
    if (fail_compile) return NULL;
    *nwords = fake_nwords;
    return dup_words(fake_words, fake_nwords);
}

static int mem_disabled, mem_set;
static uint64_t mem_key;
static uint32_t *mem_words;
static size_t mem_nwords;
static int mem_off_fn(void) { return mem_disabled; }
static int mem_has(uint64_t h) { return mem_set && mem_key == h; }
static uint32_t *mem_load(uint64_t h, size_t *nwords) {
    if (!mem_has(h)) return NULL;
    *nwords = mem_nwords;
    return dup_words(mem_words, mem_nwords);
}
static void mem_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    free(mem_words);
    mem_key = h;
    mem_words = dup_words(spv, nwords);
    mem_nwords = nwords;
    mem_set = 1;
}
static void mem_reset(void) { free(mem_words); mem_words = NULL; mem_nwords = 0; mem_key = 0; mem_set = 0; }
static const SpirvCacheOps mem_ops = {mem_off_fn, mem_has, mem_load, mem_store};

static void test_core_memory(void) {
    static const uint32_t minimal[] = {0x07230203, 0x00010000, 0, 3, 0};
    spirv_hash_set_id("0123456789abcdef");
    spirv_hash_set_impl(NULL);
    spirv_core_set_compiler(fake_compile);
    spirv_cache = &mem_ops;
    mem_disabled = 0;
    mem_reset();
    set_fake(minimal, 5);
    compiler_calls = 0;
    size_t n = 0;

    uint32_t *a = spirv_compile("frag source", STAGE_FRAGMENT, 0, 0, "fs", &n);
    assert(a && n == 5 && compiler_calls == 1 && mem_set);
    uint32_t *b = spirv_compile("frag source", STAGE_FRAGMENT, 0, 0, "fs", &n);
    assert(b && n == 5 && compiler_calls == 1);  // from the cache
    assert(!memcmp(a, b, 5 * 4));
    free(a);
    free(b);
    assert(spirv_ensure("frag source", STAGE_FRAGMENT, 0, 0) == 0);
    assert(compiler_calls == 1);

    compiler_calls = 0;
    assert(spirv_ensure("another source", STAGE_FRAGMENT, 0, 0) == 1);
    assert(compiler_calls == 1 && mem_set);
    assert(spirv_ensure("another source", STAGE_FRAGMENT, 0, 0) == 0);
    assert(compiler_calls == 1);

    compiler_calls = 0;
    mem_reset();
    fail_compile = 1;
    assert(spirv_ensure("fails", STAGE_FRAGMENT, 0, 0) == -1);
    assert(compiler_calls == 1 && !mem_set);  // nothing stored for a failed compile
    fail_compile = 0;

    // disabled: compiles every time, stores nothing (SAVAGEWIND_VK_NO_SPIRV_CACHE's behaviour)
    mem_disabled = 1;
    mem_reset();
    compiler_calls = 0;
    assert(spirv_ensure("d", STAGE_FRAGMENT, 0, 0) == 1);
    assert(spirv_ensure("d", STAGE_FRAGMENT, 0, 0) == 1);
    assert(compiler_calls == 2 && !mem_set);
    mem_disabled = 0;

    // a vertex module gets the NoContraction decoration; the same source as a fragment does not
    static const uint32_t vert[] = {
        0x07230203, 0x00010000, 0, 64, 0,
        WORD(OP_TYPE_VOID, 2), 1,
        WORD(OP_FADD, 5), 1, 20, 3, 4,
    };
    mem_reset();
    set_fake(vert, sizeof vert / sizeof vert[0]);
    uint32_t *v = spirv_compile("same source", STAGE_VERTEX, 0, 0, "vs", &n);
    assert(v && n == sizeof vert / sizeof vert[0] + 3);
    assert(v[5] == WORD(OP_DECORATE, 3) && v[6] == 20 && v[7] == DEC_NO_CONTRACTION);
    free(v);
    mem_reset();
    set_fake(vert, sizeof vert / sizeof vert[0]);
    uint32_t *f = spirv_compile("same source", STAGE_FRAGMENT, 0, 0, "fs", &n);
    assert(f && n == sizeof vert / sizeof vert[0] && f[5] == WORD(OP_TYPE_VOID, 2));
    free(f);
    puts("ok  core: memory cache, ensure codes, disabled, stage filters");
}

// The split must not have reintroduced the old double hash: ensuring a miss hashed the source twice (once for the path
// and stat, again to find the file and store it).
static int hash_calls;
static size_t hash_last_len;
static uint64_t spy_hash(uint64_t seed, const char *src, size_t *len) {
    hash_calls++;
    uint64_t h = seed;
    size_t n = 0;
    for (const char *p = src; *p; p++, n++) h = h * 131 + (uint8_t)*p;
    hash_last_len = n;
    if (len) *len = n;
    return h;
}

static void test_single_hash(void) {
    static const uint32_t minimal[] = {0x07230203, 0x00010000, 0, 3, 0};
    spirv_hash_set_id("0123456789abcdef");
    spirv_core_set_compiler(fake_compile);
    spirv_cache = &mem_ops;
    spirv_hash_set_impl(spy_hash);
    mem_disabled = 0;
    mem_reset();
    set_fake(minimal, 5);
    hash_calls = 0;
    compiler_calls = 0;
    assert(spirv_ensure("one hash please", STAGE_FRAGMENT, 0, 0) == 1);
    assert(hash_calls == 1 && hash_last_len == strlen("one hash please"));
    assert(compiler_calls == 1);
    hash_calls = 0;
    size_t n = 0;
    uint32_t *hit = spirv_compile("one hash please", STAGE_FRAGMENT, 0, 0, "fs", &n);
    assert(hit && hash_calls == 1 && compiler_calls == 1);  // hit: hashed for the path, not compiled
    free(hit);
    spirv_hash_set_impl(NULL);
    puts("ok  handoff: one hash per ensure/compile");
}

// -------------------------------------------------------------------------------------- disk cache edges
static uint64_t key_of(const char *src, int stage) {
    size_t len;
    return spirv_src_hash(src, stage, 0, 0, &len);
}

static void spz_path(char *out, size_t n, const char *src, int stage) {
    char name[96];
    snprintf(name, sizeof name, "spirv/%s/%016llx.spz", vk_cache_dir_id(spirv_hash_id()),
             (unsigned long long)key_of(src, stage));
    vk_cache_path(out, n, name);
}

static void write_spz(const char *path, const char *magic, uint32_t count_field, const uint32_t *words, size_t nwords) {
    uLongf zn = compressBound((uLong)(nwords * 4));
    uint8_t *z = malloc(8 + zn + 32);  // (32 trailing bytes: every written file is over has()'s 20-byte floor;
    assert(z);                          //  uncompress stops at the end of the stream and ignores them)
    memcpy(z, magic, 4);
    memcpy(z + 4, &count_field, 4);
    assert(compress2(z + 8, &zn, (const Bytef *)words, (uLong)(nwords * 4), 1) == Z_OK);
    memset(z + 8 + zn, 0, 32);
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(z, 1, 8 + zn + 32, f) == 8 + zn + 32);
    assert(fclose(f) == 0);
    free(z);
}

static void write_raw(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(p, 1, n, f) == n);
    assert(fclose(f) == 0);
}

static void test_disk(void) {
    static const uint32_t good[] = {0x07230203, 0x00010000, 0, 3, 0};
    spirv_hash_set_id("0123456789abcdef");
    spirv_hash_set_impl(NULL);
    spirv_core_set_compiler(fake_compile);
    spirv_cache = &spirv_cache_disk;
    mem_disabled = 0;
    set_fake(good, 5);
    const char *src = "disk round trip source";
    const int stage = STAGE_FRAGMENT;
    uint64_t h = key_of(src, stage);
    char path[512];
    spz_path(path, sizeof path, src, stage);
    remove(path);

    compiler_calls = 0;
    size_t n = 0;
    uint32_t *a = spirv_compile(src, stage, 0, 0, "fs", &n);
    assert(a && n == 5 && compiler_calls == 1);
    assert(spirv_cache_disk.has(h));
    free(a);
    uint32_t *b = spirv_compile(src, stage, 0, 0, "fs", &n);
    assert(b && n == 5 && compiler_calls == 1 && b[0] == 0x07230203);  // from disk
    free(b);
    assert(spirv_ensure(src, stage, 0, 0) == 0 && compiler_calls == 1);

    // Files of 20 bytes or fewer are not "there" (has wants > 20): ensure compiles and stores.
    write_raw(path, "SPZ1", 4);
    { size_t wn = 0; assert(spirv_cache_disk.load(h, &wn) == NULL); }
    assert(!spirv_cache_disk.has(h));
    assert(spirv_ensure(src, stage, 0, 0) == 1 && compiler_calls == 2);

    { uint8_t twenty[20]; memset(twenty, 0, sizeof twenty); write_raw(path, twenty, sizeof twenty); }
    assert(!spirv_cache_disk.has(h));
    assert(spirv_ensure(src, stage, 0, 0) == 1 && compiler_calls == 3);

    // A malformed file over 20 bytes: ensure's stat says "there" (as before the split: it never read the file), the
    // loader rejects it, and the compile that follows repairs the file.
    #define REPAIRS(what) do {                                                              \
        assert(spirv_cache_disk.has(h));                                                    \
        { size_t wn = 0; assert(spirv_cache_disk.load(h, &wn) == NULL); }                   \
        assert(spirv_ensure(src, stage, 0, 0) == 0);                                        \
        int before = compiler_calls;                                                        \
        size_t m = 0;                                                                       \
        uint32_t *fixed = spirv_compile(src, stage, 0, 0, "fs", &m);                        \
        assert(fixed && m == 5 && compiler_calls == before + 1);                            \
        free(fixed);                                                                        \
        assert(spirv_ensure(src, stage, 0, 0) == 0);                                        \
        (void)what;                                                                         \
    } while (0)

    write_spz(path, "XXXX", 5, good, 5);  // bad magic: the deflated module is fine, the container is not
    REPAIRS("bad magic");

    write_spz(path, "SPZ1", 0, good, 5);  // zero words
    REPAIRS("zero words");

    write_spz(path, "SPZ1", 17u << 20, good, 5);  // past the loader's 16M-word limit: refused before allocating
    REPAIRS("huge word count");

    { uint32_t bad[] = {0xdeadbeef, 0x00010000, 0, 3, 0};  // inflated words that are not SPIR-V
      write_spz(path, "SPZ1", 5, bad, 5); }
    REPAIRS("not SPIR-V");

    write_spz(path, "SPZ1", 8, good, 5);  // the header's count disagrees with what inflated
    REPAIRS("short payload");

    { uint8_t junk[64]; memset(junk, 0xA5, sizeof junk); memcpy(junk, "SPZ1", 4);  // not deflate at all
      write_raw(path, junk, sizeof junk); }
    REPAIRS("not deflate");

    // After the last repair, the loader accepts the file the repair stored.
    uint32_t *last = spirv_compile(src, stage, 0, 0, "fs", &n);
    assert(last && n == 5 && compiler_calls == 9);
    free(last);
    #undef REPAIRS
    puts("ok  disk: round trip, malformed files are misses, ensure repairs");
}

// -------------------------------------------------------------------------------------- environment
static void test_env_snapshot(void) {
    env_unset("SAVAGEWIND_VK_NO_SPIRV_CACHE");
    assert(spirv_cache_disk.disabled() == 0);   // read once, unset
    env_set("SAVAGEWIND_VK_NO_SPIRV_CACHE", "1");
    assert(spirv_cache_disk.disabled() == 0);   // still the value from the first call
    env_unset("SAVAGEWIND_VK_NO_SPIRV_CACHE");
    puts("ok  env: SAVAGEWIND_VK_NO_SPIRV_CACHE is read once");
}

// -------------------------------------------------------------------------------------- benchmark
static double now_ns(void) { return (double)clock() * (1e9 / CLOCKS_PER_SEC); }

// A translated shader's shape: a long main with arithmetic and a cbuffer, sized to `bytes`.
static char *make_source(size_t bytes) {
    static const char head[] =
        "#version 460\n"
        "layout(std140, set=0, binding=0) uniform CB0 { uvec4 cb0[4096]; };\n"
        "layout(location=0) in vec4 a0;\n"
        "void main() {\n  vec4 p = a0;\n";
    static const char line[] = "  p = p * cb0[0] + vec4(0.25) - p / vec4(3.0);\n";
    static const char tail[] = "  gl_Position = p;\n}\n";
    size_t hl = strlen(head), ll = strlen(line), tl = strlen(tail);
    size_t want = bytes < hl + tl ? hl + tl : bytes;
    char *s = malloc(want + ll + 1);
    assert(s);
    size_t o = 0;
    memcpy(s, head, hl), o = hl;
    while (o + ll + tl < want) memcpy(s + o, line, ll), o += ll;
    memcpy(s + o, tail, tl), o += tl;
    s[o] = 0;
    return s;
}

static void bench(void) {
    static const size_t sizes[] = {4u << 10, 64u << 10, 256u << 10};
    volatile uint64_t sink = 0;
    spirv_hash_set_id("0123456789abcdef");
    spirv_hash_set_impl(NULL);
    printf("== SPIR-V cache key (spirv_src_hash), %s ==\n", "FNV-1a");
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        char *src = make_source(sizes[k]);
        size_t bytes = strlen(src), len = 0;
        int iters = (int)(96u << 20) / (int)(bytes ? bytes : 1);
        if (iters < 50) iters = 50;
        if (iters > 200000) iters = 200000;
        double key = 1e30, old = 1e30;
        for (int rep = 0; rep < 3; rep++) {  // (min of 3: the first pass wakes the CPU and warms the cache)
            for (int i = 0; i < iters; i++) sink ^= spirv_src_hash(src, STAGE_VERTEX, 0, 0, &len);
            double t0 = now_ns();
            for (int i = 0; i < iters; i++) sink ^= spirv_src_hash(src, STAGE_VERTEX, 0, 0, &len);
            double a = (now_ns() - t0) / iters;
            for (int i = 0; i < iters; i++) sink ^= spirv_src_hash(src, STAGE_VERTEX, 0, 0, &len) ^ (uint64_t)strlen(src);
            t0 = now_ns();
            for (int i = 0; i < iters; i++) sink ^= spirv_src_hash(src, STAGE_VERTEX, 0, 0, &len) ^ (uint64_t)strlen(src);
            double b = (now_ns() - t0) / iters;
            if (a < key) key = a;
            if (b < old) old = b;
        }
        printf("  %6zu B: key %8.1f ns (%5.0f MB/s)  old key+strlen %8.1f ns (%+.0f%%)  2x (old ensure->compile) %8.1f ns\n",
               bytes, key, bytes / key * 1000.0, old, (old / key - 1) * 100, key * 2);
        if (bytes >= (60u << 10) && bytes <= (70u << 10))
            printf("  14k shaders at this size (#772): one hash %.2f s; the old ensure->compile added a second hash, %.2f s\n",
                   key * 14000 / 1e9, key * 14000 / 1e9);
        free(src);
    }
    puts("  (the single-hash handoff removes the second pass; the length returned with the key removes strlen)");
    (void)sink;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        bench();
        return 0;
    }
    static const char cache_root[] = "build/spirv_test_cache";  // (its files are removed by path before each use)
    env_set("SAVAGEWIND_CACHE_DIR", cache_root);
    env_unset("SAVAGEWIND_VK_NO_SPIRV_CACHE");

    test_hash();
    test_strip_names();
    test_no_contraction();
    test_env_snapshot();
    test_core_memory();
    test_single_hash();
    test_disk();
    mem_reset();
    puts("PASS: SPIR-V cache key, filters and ensure/compile handoff");
    return 0;
}
