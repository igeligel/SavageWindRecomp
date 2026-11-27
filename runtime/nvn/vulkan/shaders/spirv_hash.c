// The SPIR-V cache's key (spirv_hash.h). The digest and the order of the bytes are fixed: changing either renames
// every stored module, so the built-in path keeps the bytes the cache was keyed on before this was split out.
#include <string.h>

#include "spirv_hash.h"

static const char *build_id;
static SpirvHashFn hash_impl;

// The seed after the id and the switches, for each stage/opt/api12 combination a build uses. Stage is one of the
// maxwell STAGE_* values (0..5); an unexpected one is hashed on the spot instead of indexed.
enum { STAGE_SLOTS = 6 };
static uint64_t seeds[STAGE_SLOTS][2][2];
static uint8_t seeds_ready[STAGE_SLOTS][2][2];

static uint64_t fnv_bytes(uint64_t h, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)p[i]; h *= 0x100000001B3ull; }
    return h;
}

static uint64_t seed_for(int stage, int optimize, int api12) {
    unsigned st = (unsigned)stage;
    int o = optimize != 0, a = api12 != 0;
    int cached = st < (unsigned)STAGE_SLOTS;
    if (cached && seeds_ready[st][o][a]) return seeds[st][o][a];
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)stage ^ (optimize ? 0x6f707400ull : 0) ^
                 (api12 ? 0x31320000ull : 0);
    const char *id = build_id ? build_id : "";
    h = fnv_bytes(h, id, strlen(id));
    if (cached) { seeds[st][o][a] = h; seeds_ready[st][o][a] = 1; }
    return h;
}

// FNV-1a over a NUL-terminated source, with its length.
static uint64_t fnv_src(uint64_t h, const char *src, size_t *len) {
    size_t n = 0;
    for (const char *p = src; *p; p++, n++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    if (len) *len = n;
    return h;
}

void spirv_hash_set_id(const char *id) {
    build_id = id;
    memset(seeds_ready, 0, sizeof seeds_ready);  // (a different id: its seeds are not this build's)
}

const char *spirv_hash_id(void) { return build_id ? build_id : ""; }

void spirv_hash_set_impl(SpirvHashFn fn) { hash_impl = fn; }

uint64_t spirv_src_hash(const char *src, int stage, int optimize, int api12, size_t *len) {
    uint64_t h = seed_for(stage, optimize, api12);
    return hash_impl ? hash_impl(h, src, len) : fnv_src(h, src, len);
}
