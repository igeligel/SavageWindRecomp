// The GLSL -> SPIR-V step (spirv_core.h): moved out of spirv.c so it can be tested and its pieces swapped. The byte
// order of the key is the cache's, and the filters are the ones the shader caches were built with: keep them.
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../maxwell/shader.h"
#include "spirv_cache.h"
#include "spirv_core.h"
#include "spirv_hash.h"

static SpirvCompileFn compiler;
void spirv_core_set_compiler(SpirvCompileFn fn) { compiler = fn; }

// -------------------------------------------------------------------------------------- vertex NoContraction
// Every float add/sub/mul/div/fma gets NoContraction, so no compiler fuses or splits them. Depth pre-pass and colour
// pass are different shaders computing the same position, and the colour pass tests EQUAL: any difference in fusing
// leaves holes (foliage flickered black). GLSL's `precise gl_Position` only covers what glslang traces back from the
// position, which the translated register code defeats.
static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

uint32_t *spirv_no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

// -------------------------------------------------------------------------------------- debug names
// glslang writes one debug name for every register variable, ~16 % of a translated shader's SPIR-V, read by no driver.
void spirv_strip_names(uint32_t *spv, size_t *nwords) {
    size_t o = 5;
    for (size_t i = 5; i < *nwords;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc || i + wc > *nwords) return;  // (malformed: left as it is from here)
        bool name = op == 3 || op == 4 || op == 5 || op == 6 || op == 7 || op == 8 || op == 317 || op == 330;
        if (!name) { memmove(spv + o, spv + i, wc * 4); o += wc; }
        i += wc;
    }
    *nwords = o;
}

// -------------------------------------------------------------------------------------- cache + compile
// skip_load: the caller already knows the module is not in the cache (spirv_ensure) and does not open it again.
static uint32_t *compile_hashed(uint64_t h, const char *src, size_t len, int stage, int optimize, int api12,
                                const char *name, size_t *nwords, int skip_load) {
    const SpirvCacheOps *io = spirv_cache;
    int off = io->disabled();
    uint32_t *out = off || skip_load ? NULL : io->load(h, nwords);
    if (out) return out;
    if (!compiler) return NULL;  // (the adapter installs it before the first compile)
    out = compiler(src, len, stage, optimize, api12, name, nwords);
    if (!out) return NULL;
    spirv_strip_names(out, nwords);
    if (stage == STAGE_VERTEX) out = spirv_no_contraction(out, nwords);
    if (!off) io->store(h, out, *nwords);
    return out;
}

uint32_t *spirv_compile(const char *src, int stage, int optimize, int api12, const char *name, size_t *nwords) {
    size_t len = 0;
    uint64_t h = spirv_src_hash(src, stage, optimize, api12, &len);
    return compile_hashed(h, src, len, stage, optimize, api12, name, nwords, 0);
}

int spirv_ensure(const char *src, int stage, int optimize, int api12) {
    size_t len = 0;
    uint64_t h = spirv_src_hash(src, stage, optimize, api12, &len);  // (once: the miss compiles from it)
    const SpirvCacheOps *io = spirv_cache;
    if (!io->disabled() && io->has(h)) return 0;
    size_t n = 0;
    uint32_t *spv = compile_hashed(h, src, len, stage, optimize, api12, "precompile", &n, 1);
    free(spv);
    return spv ? 1 : -1;
}
