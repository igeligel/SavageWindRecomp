// Where the shader caches live and how a build's id becomes a folder name. Split from shaders.c so the SPIR-V cache
// (spirv_cache.c) and its tests do not need a Vulkan device; the declarations stay in shaders.h for the runtime.
#pragma once
#include <stddef.h>

const char *vk_cache_dir(void);               // cache/ (or SAVAGEWIND_CACHE_DIR), made when missing
void vk_cache_path(char *out, size_t n, const char *name);  // that folder + "/" + name
const char *vk_cache_dir_id(const char *id);  // a folder name for a translator / SPIR-V id
