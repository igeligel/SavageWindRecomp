// The SPIR-V disk cache (spirv_cache.h), moved out of spirv.c so the compile step can be tested and swapped without
// Vulkan. The file format and the path are part of the shader caches' contract (SPIRV_ID): keep them as they were.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "cache_path.h"
#include "spirv_cache.h"
#include "spirv_hash.h"

static void spirv_path(char *path, size_t n, uint64_t h) {
    char name[96];
    snprintf(name, sizeof name, "spirv/%s/%016llx.spz", vk_cache_dir_id(spirv_hash_id()), (unsigned long long)h);
    vk_cache_path(path, n, name);
}

// Read once: SAVAGEWIND_VK_NO_SPIRV_CACHE has to mean the same for lookup and store in one run (a shader compiled
// while it changed would be stored for a key the cache does not use again).
static int disk_disabled(void) {
    static int v = -1;
    if (v < 0) v = getenv("SAVAGEWIND_VK_NO_SPIRV_CACHE") != NULL;
    return v;
}

static int disk_has(uint64_t h) {
    char path[512];
    struct stat st;
    spirv_path(path, sizeof path, h);
    return !stat(path, &st) && st.st_size > 20;
}

static uint32_t *disk_load(uint64_t h, size_t *nwords) {
    char path[512];
    spirv_path(path, sizeof path, h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *z = n > 8 && n < (64l << 20) ? malloc((size_t)n) : NULL;
    if (z && fread(z, 1, (size_t)n, f) != (size_t)n) { free(z); z = NULL; }
    fclose(f);
    uint32_t *d = NULL, words = 0;
    if (z && !memcmp(z, "SPZ1", 4)) {
        memcpy(&words, z + 4, 4);
        uLongf len = (uLongf)words * 4;
        d = words && words < (16u << 20) ? malloc(len) : NULL;
        if (d && (uncompress((Bytef *)d, &len, z + 8, (uLong)(n - 8)) != Z_OK || len != (uLongf)words * 4 || d[0] != 0x07230203)) {
            free(d);
            d = NULL;
        }
    }
    free(z);
    if (d) *nwords = words;
    return d;
}

static void disk_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    static int made;
    char path[512], tmp[540], name[96];
    if (!made) {
        made = 1;
        vk_cache_path(path, sizeof path, "spirv");
        mkdir(path, 0755);
        snprintf(name, sizeof name, "spirv/%s", vk_cache_dir_id(spirv_hash_id()));
        vk_cache_path(path, sizeof path, name);
        mkdir(path, 0755);
    }
    spirv_path(path, sizeof path, h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&made ^ (unsigned long long)(uintptr_t)spv);
    uLongf zn = compressBound((uLong)(nwords * 4));
    uint8_t *z = malloc(8 + zn);
    if (!z) return;
    memcpy(z, "SPZ1", 4);
    uint32_t w = (uint32_t)nwords;
    memcpy(z + 4, &w, 4);
    bool ok = compress2(z + 8, &zn, (const Bytef *)spv, (uLong)(nwords * 4), 1) == Z_OK;  // (fast: stores happen at draws too)
    FILE *f = ok ? fopen(tmp, "wb") : NULL;
    ok = f && fwrite(z, 1, 8 + zn, f) == 8 + zn;
    if (f && fclose(f)) ok = false;
    free(z);
    if (f && (!ok || rename(tmp, path) != 0)) remove(tmp);  // (another thread may have stored it first)
}

const SpirvCacheOps spirv_cache_disk = {disk_disabled, disk_has, disk_load, disk_store};
const SpirvCacheOps *spirv_cache = &spirv_cache_disk;
