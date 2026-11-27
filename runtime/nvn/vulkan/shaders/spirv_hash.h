// The key of a stored SPIR-V module: FNV-1a of the GLSL source, seeded by the build's SPIR-V id and the switches that
// change the module (stage, spirv-opt, the Vulkan env). Split from spirv.c so it can be swapped (tests count what is
// hashed, benchmarks compare kernels) and so the length comes back with the digest: the compiler needs it and the
// source is not scanned twice.
#pragma once
#include <stddef.h>
#include <stdint.h>

// seed: the source hash's starting point (already includes the id and the stage/opt/api12 bits); *len: the source's
// length. An impl must produce the same digest for the same seed and bytes: it is the name of every stored module.
typedef uint64_t (*SpirvHashFn)(uint64_t seed, const char *src, size_t *len);

void spirv_hash_set_id(const char *id);    // the build's SPIR-V id (vk_spirv_id), before the first hash
const char *spirv_hash_id(void);
void spirv_hash_set_impl(SpirvHashFn fn);  // NULL: the built-in FNV-1a (the cache's)

// The source's hash for a stage, the spirv-opt setting and a Vulkan 1.2 driver (they are part of the key: the same
// source is a different module for each), and its length. The seed of the id+switches is computed once per build.
uint64_t spirv_src_hash(const char *src, int stage, int optimize, int api12, size_t *len);
