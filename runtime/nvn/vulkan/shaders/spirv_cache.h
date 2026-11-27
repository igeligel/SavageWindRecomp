// The SPIR-V disk cache (cache/spirv/<id>/<key>.spz): a known module costs a file read instead of a GLSL compile (the
// driver's pipeline cache only covers the step after this one). A file: "SPZ1", the module's word count, the module
// deflated (zlib; a third to a quarter of the size: the precompiled shaders of the whole game take ~0.4 GB instead
// of 1.4). The operations are a table so lookup and store share one key computation and a test can swap in memory.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct SpirvCacheOps {
    int (*disabled)(void);                             // SAVAGEWIND_VK_NO_SPIRV_CACHE (read once)
    int (*has)(uint64_t h);                            // the module's file is there
    uint32_t *(*load)(uint64_t h, size_t *nwords);     // malloc'd, NULL on a miss or a bad file
    void (*store)(uint64_t h, const uint32_t *spv, size_t nwords);
} SpirvCacheOps;

extern const SpirvCacheOps *spirv_cache;  // (&spirv_cache_disk unless a test swaps it)
extern const SpirvCacheOps spirv_cache_disk;
