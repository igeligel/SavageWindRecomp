// The GLSL -> SPIR-V step without Vulkan: the cache key and lookup, the compile (through a swappable compiler), the
// vertex-shader post-processing and the store. spirv.c is the Vulkan / shaderc adapter; this stays testable without a
// device.
#pragma once
#include <stddef.h>
#include <stdint.h>

// The compiler for one stage. len is the source's length (from the hash: spirv_hash.c); api12 selects SPIR-V 1.5
// instead of 1.6. Returns malloc'd words, NULL on a compile error.
typedef uint32_t *(*SpirvCompileFn)(const char *src, size_t len, int stage, int optimize, int api12,
                                    const char *name, size_t *nwords);
void spirv_core_set_compiler(SpirvCompileFn fn);  // (the shaderc one: spirv.c, once at startup)

// The SPIR-V of src (from the cache, else compiled now and stored), or NULL. Its key is hashed once.
uint32_t *spirv_compile(const char *src, int stage, int optimize, int api12, const char *name, size_t *nwords);
// The same step for the whole-game precompile: 0 was in the cache, 1 compiled now, -1 failed. Hashes once and does not
// load a module it already checked for.
int spirv_ensure(const char *src, int stage, int optimize, int api12);

// Vertex shaders: every float add/sub/mul/div/fma gets NoContraction so no compiler fuses or splits them. (Exposed for
// tests.) Returns the module (the same pointer when nothing was added) and updates *nwords.
uint32_t *spirv_no_contraction(uint32_t *spv, size_t *nwords);
// Debug names (OpSource, OpName, ...: ~16 % of a translated shader's SPIR-V, read by no driver) removed in place.
void spirv_strip_names(uint32_t *spv, size_t *nwords);
