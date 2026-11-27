// The shader caches' folder (moved out of shaders.c so a test can use it without Vulkan). The folder is created here,
// once, from the same environment the runtime reads everywhere else.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cache_path.h"
#include "hash.h"

// The shader and pipeline caches live in cache/ under the data root (the working directory), or in the folder
// SAVAGEWIND_CACHE_DIR names (tests that must not touch the player's caches).
const char *vk_cache_dir(void) {
    static const char *dir;
    if (!dir) {
        const char *e = getenv("SAVAGEWIND_CACHE_DIR");
        dir = e && *e ? e : "cache";
        mkdir(dir, 0755);
    }
    return dir;
}

void vk_cache_path(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", vk_cache_dir(), name);
}

// A cache folder's name for an id (TRANSLATOR_ID / SPIRV_ID are 16 hex digits; built without the Makefile, a date)
const char *vk_cache_dir_id(const char *id) {
    static __thread char out[2][20];
    static __thread int k;
    char *o = out[k++ & 1];
    bool hex = strlen(id) == 16;
    for (const char *p = id; *p && hex; p++) hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
    if (hex) snprintf(o, 20, "%s", id);
    else snprintf(o, 20, "%016llx", (unsigned long long)hash64(0xcbf29ce484222325ull, id, strlen(id)));
    return o;
}
