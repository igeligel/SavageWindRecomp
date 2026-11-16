// The per-use cost of deciding whether a CPU-owned texture changed, the hot path of textures.c's upload_texture.
//
// The workload is modeled on the [vk] log lines in issue #589: about 50 textures are looked at in a frame in the
// open world, most are static compressed textures, a few are written by the CPU (video, menus) and the every-frame
// path full-hashes those. Draw/upload counts are from that log; the texture sizes are the usual BOTW mix.
//
// Two binaries are built from the same module: this one (a long-stable dynamic texture is demoted to a full hash
// every 8th frame) and test_textures_upload_baseline (-DTEX_WATCH_STABLE_FRAMES huge: the old "once dynamic,
// dynamic forever, full hash every frame" path).
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tex_watch.h"

#define KB (1u << 10)
#define MB (1u << 20)

typedef struct {
    const char *name;
    uint64_t size;
    bool pitch;     // flags & 0x10: written by the CPU every frame (video)
    int change_at;  // -1 never, -2 every frame, else the frame it is written once
    int changed_count;
    TexWatch w;
    uint8_t *mem;
} Tex;

typedef struct {
    uint64_t full_bytes, sample_bytes;
    uint64_t full, sampled, changed;
} Acc;

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void fill_pattern(uint8_t *p, uint64_t n, uint32_t seed) {
    uint32_t x = seed ? seed : 1;
    for (uint64_t i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; p[i] = (uint8_t)(x >> 24); }
}

// A changed row or a few texels: what a menu/video write looks like; the full hash still reads every byte.
static void poke(uint8_t *p, uint64_t n, uint32_t v) {
    if (!n) return;
    uint64_t at = (uint64_t)v * 2654435761u % n;
    for (uint64_t i = 0; i < 16 && at + i < n; i++) p[at + i] ^= (uint8_t)(v + i * 7 + 1);
}

static bool decay_on(void) { return TEX_WATCH_STABLE_FRAMES < 1000u; }

static Tex *workload(const char *which, int *ntex) {
    // 50 textures: 46 static, 2 pitch video (cutscene only), 2 one-shot (a menu/map update).
    static const uint64_t sizes[] = {16 * KB, 64 * KB, 256 * KB, 512 * KB, 1 * MB, 64 * KB, 128 * KB, 2 * MB};
    const int n = 50;
    Tex *t = calloc((size_t)n, sizeof *t);
    bool cutscene = !strcmp(which, "cutscene");
    bool churn = !strcmp(which, "churn");
    for (int i = 0; i < n; i++) {
        t[i].size = sizes[i % (int)(sizeof sizes / sizeof sizes[0])];
        t[i].change_at = -1;
        if (cutscene && (i == 46 || i == 47)) {  // 1280x720 and 1920x1080 RGBA8: the game's videos
            t[i].size = i == 46 ? 1280ull * 720 * 4 : 1920ull * 1080 * 4;
            t[i].pitch = true;
            t[i].change_at = -2;
        } else if (i >= 48) {  // the save slots' pictures / map: written once, then static
            t[i].size = i == 48 ? 4 * MB : 2 * MB;
            t[i].change_at = 30;
        }
        if (churn && i < 8) {  // a menu that repaints eight large textures once
            t[i].size = 4 * MB;
            t[i].change_at = 20 + i * 5;
        }
        t[i].name = which;
        t[i].mem = malloc(t[i].size);
        assert(t[i].mem);
        fill_pattern(t[i].mem, t[i].size, (uint32_t)(i + 1));
    }
    *ntex = n;
    return t;
}

static void reset_workload(Tex *t, int n) {
    for (int i = 0; i < n; i++) {
        memset(&t[i].w, 0, sizeof t[i].w);
        t[i].changed_count = 0;
        fill_pattern(t[i].mem, t[i].size, (uint32_t)(i + 1));
    }
}

static void free_workload(Tex *t, int n) {
    for (int i = 0; i < n; i++) free(t[i].mem);
    free(t);
}

// One frame: every texture is used once, exactly as vk_tex -> upload_texture does it.
static void frame(Tex *t, int n, uint64_t f, Acc *a) {
    uint64_t budget = 4 * MB;
    for (int i = 0; i < n; i++) {
        if (t[i].change_at == -2) poke(t[i].mem, t[i].size, (uint32_t)f + 1);
        else if (t[i].change_at == (int)f) poke(t[i].mem, t[i].size, (uint32_t)f + 1);
        if (!tex_watch_gate(&t[i].w, false, f, (uintptr_t)t[i].mem)) continue;
        bool force = false;
        if (!t[i].w.dynamic && t[i].w.uploaded && f - t[i].w.full_frame >= 120 && budget >= t[i].size) {
            force = true;
            budget -= t[i].size;
        }
        TexWatchResult r = tex_watch_check(&t[i].w, t[i].mem, t[i].size, false, t[i].pitch, force, f);
        a->full += r.full;
        a->full_bytes += r.bytes_full;
        a->sampled += r.sampled;
        a->sample_bytes += r.bytes_sampled;
        a->changed += r.changed;
        if (r.changed) t[i].changed_count++;
    }
}

static void run(const char *which, int frames, bool bench) {
    int n;
    Tex *t = workload(which, &n);
    reset_workload(t, n);
    Acc a = {0};
    double t0 = now_ns();
    for (int f = 0; f < frames; f++) frame(t, n, (uint64_t)f, &a);
    double dt = now_ns() - t0;

    if (bench) {
        printf("  %-8s %5d frames %2d textures   full %6.2f MB/f (%5llu)  sample %5.2f MB/f (%5llu)  "
               "%6.3f ms/f (%.2f ns/use)\n",
               which, frames, n, (double)a.full_bytes / frames / MB, (unsigned long long)a.full,
               (double)a.sample_bytes / frames / MB, (unsigned long long)a.sampled, dt / 1e6 / frames,
               dt / (double)frames / n);
    }

    // The checks the optimization must not break.
    for (int i = 0; i < n; i++) {
        if (t[i].change_at == -1) {
            assert(t[i].changed_count == 1);  // the initial upload is the only change
            assert(!t[i].w.dynamic);
        } else if (t[i].change_at == -2) {
            assert(t[i].changed_count >= frames - 1);
            assert(t[i].w.dynamic);
        }
    }
    Tex *one = NULL;
    for (int i = 0; i < n; i++) if (t[i].change_at == 30) one = &t[i];
    if (one) {
        assert(one->w.uploaded);
        assert(one->changed_count >= 2 && one->changed_count <= 3);  // initial + the one write
        if (decay_on()) {
            assert(!one->w.dynamic);  // 30 unchanged full hashes and it is periodic again
            // And a later write is still caught within the every-8-frames gate.
            poke(one->mem, one->size, 0x5eed);
            bool found = false;
            for (int f = frames; f < frames + 8; f++) {
                Acc b = {0};
                frame(t, n, (uint64_t)f, &b);
                if (b.changed) found = true;
            }
            assert(found);
        } else {
            assert(one->w.dynamic);  // baseline: stays dynamic forever
        }
    }
    free_workload(t, n);
}

static void prims(void) {
    static const uint64_t sizes[] = {16 * KB, 256 * KB, 1 * MB, 4 * MB, 8 * MB};
    printf("  hash throughput (GB/s):\n");
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        uint64_t n = sizes[i];
        uint8_t *p = malloc(n);
        assert(p);
        fill_pattern(p, n, 7);
        const int it = n > 4 * MB ? 40 : n > 1 * MB ? 100 : 1000;
        volatile uint64_t sink = 0;
        double t0 = now_ns();
        for (int k = 0; k < it; k++) sink ^= tex_watch_hash_full(p, n);
        double full = (double)n * it / (now_ns() - t0);
        t0 = now_ns();
        for (int k = 0; k < it; k++) sink ^= tex_watch_hash_sample(p, n);
        double sample = (double)n * it / (now_ns() - t0);
        printf("    %5llu KiB  full %6.1f  sample %6.1f\n", (unsigned long long)(n / KB), full, sample);
        (void)sink;
        free(p);
    }
}

int main(int argc, char **argv) {
    bool bench = argc > 1 && !strcmp(argv[1], "--bench");
    printf("texture upload watch: periodic verify %s\n", decay_on() ? "on" : "off (baseline)");
    run("open", 600, bench);
    run("cutscene", 600, bench);
    run("churn", 600, bench);
    if (bench) prims();
    return 0;
}
