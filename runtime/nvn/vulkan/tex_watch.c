#include "tex_watch.h"

#include <string.h>

// Four independent lanes (one serial multiply chain ran at ~1.5 bytes/cycle; dynamic textures are hashed every
// frame). Values are only compared within a run.
uint64_t tex_watch_hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

// 8 bytes at each of up to 2048 places (a byte at each of 4096 was a cache miss per byte: ~50 textures a frame are
// sampled in the open world).
uint64_t tex_watch_hash_sample(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    uint64_t words = n / 8, step = words > 2048 ? words / 2048 : 1;
    for (uint64_t i = 0; i < words; i += step) { uint64_t w; memcpy(&w, p + i * 8, 8); h = (h ^ w) * 1099511628211ull; h ^= h >> 29; }
    for (uint64_t i = words * 8; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

TexWatchResult tex_watch_check(TexWatch *w, const uint8_t *mem, uint64_t total, bool written, bool pitch,
                               bool force_full, uint64_t frame) {
    TexWatchResult r = {.checked = true};
    w->checked_frame = frame;
    // A pitch/linear texture is written by the CPU (video, decoded frames): keep it on the every-frame path.
    if (pitch) { w->dynamic = true; w->verify = false; }
    r.dynamic = w->dynamic;
    // A demoted texture is only looked at on the every-8-frames gate, and then fully hashed: the sample could
    // miss a small write, and a menu that repaints one row must not stay stale for long.
    bool full = w->dynamic || !w->uploaded || force_full || w->verify;
    if (!full) {
        r.sampled = true;
        r.bytes_sampled = total <= 2048 * 8 ? total : 2048 * 8;
        if (tex_watch_hash_sample(mem, total) == w->sample_hash) return r;  // unchanged
    }
    r.full = true;
    r.bytes_full = total;
    uint64_t h = tex_watch_hash_full(mem, total);
    w->full_frame = frame;
    w->sample_hash = tex_watch_hash_sample(mem, total);
    if (w->uploaded && h == w->data_hash) {
        if (!pitch && w->dynamic && ++w->stable_frames >= TEX_WATCH_STABLE_FRAMES) {
            w->dynamic = false;
            w->verify = true;
        }
        return r;  // unchanged
    }
    w->stable_frames = 0;
    w->verify = false;
    if (w->uploaded && !written) w->dynamic = true;
    w->data_hash = h;
    w->uploaded = true;
    r.changed = true;
    return r;
}
