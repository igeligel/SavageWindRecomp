// GLSL -> SPIR-V: shaderc, this build's SPIR-V id, and the switches the cache key needs (g_vk.api12, spirv-opt). The
// key, the disk cache, the vertex-shader post-processing and the ensure/compile handoff are in shaders/ (spirv_core.c,
// spirv_hash.c, spirv_cache.c), split out so they are testable and swappable without a Vulkan device or shaderc. What
// this file makes is part of every shader cache's key (vk_spirv_id: a hash of this file and those, from the Makefile),
// so it holds only that step.
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>

#include "shaders.h"
#include "shaders/spirv_core.h"
#include "shaders/spirv_hash.h"

#ifndef SPIRV_ID
#define SPIRV_ID __DATE__ " " __TIME__  // (built without the Makefile's hash: valid for this build only)
#endif
const char *vk_spirv_id(void) { return SPIRV_ID; }

// spirv-opt for this stage. Off by default (see vk_glsl_to_spirv). Fragment shaders get it on AMD's own shader compiler
// (Windows, the GPUs before RDNA 3; device.c): it draws the sky wrong from our unoptimized SPIR-V, on RX 6600 and
// 6700 XT and a Vega 7 (#359, #665), while the same driver's LLPC (RDNA 3 and later) and every other driver draw it
// right; SAVAGEWIND_VK_OPT=1 fixed it on an RX 6600. Vertex shaders stay unoptimized, for the depth pre-pass's match.
// SAVAGEWIND_VK_OPT=1: every stage. A stage's setting is part of its cache key (src_hash): a run with it on found the
// unoptimized SPIR-V of a run with it off, and the sky came back wrong.
static bool opt_fragments;
void vk_spirv_optimize_fragments(bool on) { opt_fragments = on; }
static bool optimized(int stage) {
    static int all = -1;
    if (all < 0) all = getenv("SAVAGEWIND_VK_OPT") != NULL;
    return all || (opt_fragments && stage == STAGE_FRAGMENT);
}

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
}

// The compiler spirv_core calls. One per thread (a shared one behind a lock serialised all the pipeline workers). No
// spirv-opt by default: it rewrote the depth pre-pass and colour-pass vertex shaders differently enough that their
// depths stopped matching (EQUAL test: black holes in foliage), and it was most of the compile time; the driver
// optimises anyway. SAVAGEWIND_VK_OPT=1 turns it back on (experiments).
static uint32_t *compile_shaderc(const char *src, size_t len, int stage, int optimize, int api12, const char *name,
                                 size_t *nwords) {
    static __thread shaderc_compiler_t comp;
    if (!comp) comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan,
                                           api12 ? shaderc_env_version_vulkan_1_2 : shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(o, optimize ? shaderc_optimization_level_performance
                                                              : shaderc_optimization_level_zero);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, len, kind_of(stage), name, "main", o);
    shaderc_compile_options_release(o);
    uint32_t *out = NULL;
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        static int nlog;
        if (nlog++ < 20) LOG("[shader] %s: GLSL compile failed:\n%s\n%s\n", name, shaderc_result_get_error_message(r), src);
    } else {
        size_t n = shaderc_result_get_length(r);
        out = malloc(n);
        if (out) { memcpy(out, shaderc_result_get_bytes(r), n); *nwords = n / 4; }
    }
    shaderc_result_release(r);
    return out;
}

// The id and the compiler, once: the key needs the id before the first shader (and before any cache path is built).
static pthread_once_t install_once = PTHREAD_ONCE_INIT;
static void install(void) {
    spirv_hash_set_id(vk_spirv_id());
    spirv_core_set_compiler(compile_shaderc);
}

// The SPIR-V of src (from the disk cache, compiled if missing): 0 it was there, 1 compiled, -1 failed.
int vk_spirv_ensure(const char *src, int stage) {
    pthread_once(&install_once, install);
    return spirv_ensure(src, stage, optimized(stage), g_vk.api12);
}

// SPIR-V words of src, or NULL (error logged with the source). *nwords set on success. Thread-safe: one shaderc
// compiler per thread (a shared one behind a lock serialised all the pipeline workers).
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords) {
    pthread_once(&install_once, install);
    return spirv_compile(src, stage, optimized(stage), g_vk.api12, name, nwords);
}
