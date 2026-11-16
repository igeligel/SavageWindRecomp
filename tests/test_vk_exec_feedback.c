// Tests and benchmark for runtime/nvn/vulkan/exec_feedback.c: the per-draw attachment membership scan that
// resolve_feedback runs over a draw's bound textures. No Vulkan device or game data: synthetic bound textures and
// render targets, with the draw counts from the issue logs.
//
//   make test-vk-exec-feedback    checks the old and new membership answers agree
//   make bench-vk-exec-feedback   compares the old per-texture scan with the precomputed set
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../runtime/nvn/vulkan/exec_feedback.c"

static volatile uint64_t sink;
static uint32_t rng = 0x12345678u;

static uint32_t rnd(void) {
    uint32_t x = rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng = x;
}

#define NTEX 64
#define NDRAW 7382
#define MAXNTEX 16
static NvnTexture tex[NTEX];
static TexBackend be[NTEX];
VkG g_vk;

typedef struct {
    uint8_t ntex, ncolor, det, attach_pct;
    NvnTexture *bound[MAXNTEX];
    NvnTexture *color[8];
    NvnTexture *depth;
} Draw;

static Draw draws[2][NDRAW];
static const uint32_t smask[2] = {0xFF, 0xFF};

static int pick_ntex(void) {
    uint32_t r = rnd() % 100;
    if (r < 45) return 1 + (int)(rnd() % 2);
    if (r < 75) return 3 + (int)(rnd() % 3);
    if (r < 92) return 6 + (int)(rnd() % 3);
    return 9 + (int)(rnd() % 8);
}

static int pick_ncolor(void) {
    uint32_t r = rnd() % 100;
    if (r < 40) return 1;
    if (r < 75) return 2 + (int)(rnd() % 3);
    if (r < 95) return 5 + (int)(rnd() % 4);
    return 8;
}

static void make_draws(int w, int attach_pct) {
    for (int d = 0; d < NDRAW; d++) {
        Draw *x = &draws[w][d];
        x->ntex = (uint8_t)pick_ntex();
        x->ncolor = (uint8_t)pick_ncolor();
        x->det = (uint8_t)(rnd() & (rnd() % 100 < 30 ? 0xFF : 0));
        x->attach_pct = (uint8_t)attach_pct;
        for (int i = 0; i < x->ncolor; i++) x->color[i] = &tex[48 + i];
        x->depth = &tex[56];
        for (int i = 0; i < x->ntex; i++) {
            if ((int)(rnd() % 100) < attach_pct) {
                if (rnd() % 8 == 0) x->bound[i] = x->depth;
                else x->bound[i] = x->color[rnd() % x->ncolor];
            } else {
                x->bound[i] = &tex[rnd() % 40];
            }
        }
    }
}

static bool old_same_image(NvnTexture *a, NvnTexture *b) {
    static int by_obj = -1;
    if (by_obj < 0) by_obj = getenv("SAVAGEWIND_VK_SNAP_BY_OBJECT") ? atoi(getenv("SAVAGEWIND_VK_SNAP_BY_OBJECT")) != 0 : !g_vk.is_mali;
    return a == b || (!by_obj && a && b && a->backend && a->backend == b->backend);
}

static bool old_is_attachment(const Draw *x, NvnTexture *t) {
    if (!t) return false;
    if (old_same_image(t, x->depth)) return true;
    for (int i = 0; i < x->ncolor; i++)
        if (old_same_image(t, x->color[i]) && !(x->det & (1u << i))) return true;
    return false;
}

static double now_ns(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static double run(bool old, int w, int draws_per_frame, int frames) {
    uint64_t t0 = now_ns();
    for (int f = 0; f < frames; f++)
        for (int d = 0; d < draws_per_frame; d++) {
            const Draw *x = &draws[w][d];
            if (old) {
                for (int i = 0; i < x->ntex; i++) sink += old_is_attachment(x, x->bound[i]);
            } else {
                feedback_attachment_begin(x->depth, x->color, x->ncolor, x->det, false, false, true, 0, 0, 0, smask);
                for (int i = 0; i < x->ntex; i++) sink += feedback_is_attachment(x->bound[i]);
            }
        }
    return (now_ns() - t0) / (double)(frames * draws_per_frame);
}

static void compare(const char *name, int w, int draws_per_frame) {
    double old = run(true, w, draws_per_frame, 20);
    double new_ = run(false, w, draws_per_frame, 20);
    printf("  %-24s %5d draws  old %7.2f ns/draw  new %6.2f ns/draw  %6.1f%%   old %6.3f ms/frame  new %6.3f ms/frame\n",
           name, draws_per_frame, old, new_, (new_ / old - 1) * 100, old * draws_per_frame / 1e6,
           new_ * draws_per_frame / 1e6);
}

int main(int argc, char **argv) {
    for (int i = 0; i < NTEX; i++) {
        memset(&tex[i], 0, sizeof tex[i]);
        memset(&be[i], 0, sizeof be[i]);
        tex[i].backend = &be[i];
    }
    g_vk.is_mali = false;
    make_draws(0, 5);   // (typical: most bound textures are not attachments)
    make_draws(1, 25);  // (feedback-heavy: many sampled attachments)

    // The cached set answers exactly what the old scan did.
    for (int w = 0; w < 2; w++)
        for (int d = 0; d < NDRAW; d++) {
            const Draw *x = &draws[w][d];
            feedback_attachment_begin(x->depth, x->color, x->ncolor, x->det, false, false, true, 0, 0, 0, smask);
            for (int i = 0; i < x->ntex; i++) assert(old_is_attachment(x, x->bound[i]) == feedback_is_attachment(x->bound[i]));
        }

    if (!(argc > 1 && !strcmp(argv[1], "--bench"))) {
        puts("test_vk_exec_feedback: ok");
        return 0;
    }

    printf("exec_feedback (per-draw attachment membership, draw counts from issues #332/#589)\n");
    compare("typical, 5% attachments", 0, 3663);
    compare("typical, 5% attachments", 0, 7382);
    compare("feedback-heavy, 25%", 1, 3663);
    compare("feedback-heavy, 25%", 1, 7382);

    return 0;
}
