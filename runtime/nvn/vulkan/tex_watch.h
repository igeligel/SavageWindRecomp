// Change detection for CPU-owned textures: hashes guest memory and decides whether the GPU copy is stale.
// The decision was moved out of textures.c's upload_texture so it can be measured without a GPU
// (tests/test_textures_upload.c); textures.c still owns the Vulkan upload itself.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// A texture that changed once and then stayed still (a menu, a map, a screenshot) used to stay "dynamic"
// forever and be fully hashed every frame. After this many unchanged full hashes it is verified on the
// every-8-frames gate instead (a full hash, not the sample), so any later write is still caught within 8 frames.
// tests/test_textures_upload_baseline builds with a huge value to measure the old behaviour.
#ifndef TEX_WATCH_STABLE_FRAMES
#define TEX_WATCH_STABLE_FRAMES 30u
#endif

typedef struct {
    uint64_t data_hash;      // hash_full of the contents last uploaded
    uint64_t sample_hash;    // cheap sample of those contents
    uint64_t checked_frame;  // last frame it was looked at (once a frame)
    uint64_t full_frame;     // last frame it was fully hashed
    uint32_t stable_frames;  // consecutive full hashes that matched (dynamic is demoted after a while)
    bool uploaded;           // the contents are on the GPU
    bool dynamic;            // check every frame: known to change (or changed without a write notice)
    bool verify;             // was dynamic, now fully hashed every 8th frame (catches any later write)
} TexWatch;

typedef struct {
    bool checked;       // the caller's checks went through (the texture is readable guest memory)
    bool sampled;       // fnv_sample ran
    bool full;          // hash_full ran
    bool changed;       // the contents differ from what was uploaded
    bool dynamic;       // w->dynamic when the full hash was counted (for the tex/full hash (dynamic) tally)
    uint64_t bytes_sampled;  // bytes read by the sample (at most 16 KiB)
    uint64_t bytes_full;     // bytes read by the full hash
} TexWatchResult;

// The cheap once-a-frame and every-8-frames gates. Kept here so upload_texture and the benchmark agree.
static inline bool tex_watch_gate(const TexWatch *w, bool written, uint64_t frame, uintptr_t salt) {
    if (!written && w->uploaded && w->checked_frame == frame) return false;
    if (!written && w->uploaded && !w->dynamic && ((frame + (salt >> 6)) & 7)) return false;
    return true;
}

TexWatchResult tex_watch_check(TexWatch *w, const uint8_t *mem, uint64_t total, bool written, bool pitch,
                               bool force_full, uint64_t frame);

uint64_t tex_watch_hash_full(const uint8_t *p, uint64_t n);
uint64_t tex_watch_hash_sample(const uint8_t *p, uint64_t n);
