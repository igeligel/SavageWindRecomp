// Tests and benchmark for runtime/nvn/vulkan/exec_post_hook.c: the once-a-frame feature flags and the per-draw
// post/frame-generation gate. No Vulkan device or game data: the feature checks and the heavy bodies are stubs, so this
// measures the checks a draw with the effects off no longer runs.
//
//   make test-vk-exec-post    checks the flags and the copy-pending hand-off
//   make bench-vk-exec-post   compares the old per-draw checks with the new bool gate at realistic draw counts
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../runtime/nvn/vulkan/exec_post_hook.c"

#define STUB __attribute__((noinline))

static volatile uint64_t sink;
static uint64_t frame;
static bool mvec_on, fsr3_on, dlss_on, grade_on;
static int heavy_calls, copy_calls;
static uint32_t jitter_phases;

Settings g_settings;
volatile int g_fg_want;

STUB uint64_t renderer_present_count(void) { return frame; }
STUB bool vk_dlss_available(void) { return dlss_on; }
STUB bool rt_fsr3(void) { return fsr3_on; }
STUB int rt_antialias(void) { return g_settings.antialias; }
STUB bool rt_dlss(void) { return g_settings.upscaler == UPSCALE_DLSS && vk_dlss_available(); }
STUB bool rt_temporal(void) { return rt_antialias() == AA_TAA || rt_fsr3() || rt_dlss(); }
STUB void rt_jitter_set_phases(uint32_t n) { jitter_phases = n ? n : 8; }

// The frame-cached part of mvec.c's function (the old gate called it per draw; the new module once a frame).
STUB bool mvec_enabled(void) {
    static uint64_t at = ~0ull;
    static bool on;
    if (at != frame) at = frame, on = mvec_on || rt_temporal();
    return on;
}

STUB bool vk_exec_grade_wanted(VkImageView *lut, float pc[16]) {
    if (lut) *lut = VK_NULL_HANDLE;
    if (pc) memset(pc, 0, 16 * sizeof pc[0]);
    return grade_on;
}
void vk_exec_scene_post_do(Prog *pr, CmdRec *r) { (void)pr; (void)r; heavy_calls++; }
void vk_exec_scene_post_off(void) {}
void vk_exec_fg_draw_do(Prog *pr, CmdRec *r) { (void)pr; (void)r; heavy_calls++; }
void vk_exec_fg_copy_hudless_do(void) { copy_calls++; }

static double now_ns(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

// The old scene_post/fg_draw checks, as they were before the module: run for every draw.
static bool old_hook(void) {
    static int fsr_on = -1, cas_on = -1;
    if (fsr_on < 0) fsr_on = getenv("SAVAGEWIND_SCENE_FSR") ? atoi(getenv("SAVAGEWIND_SCENE_FSR")) : 1;
    if (cas_on < 0) cas_on = getenv("SAVAGEWIND_SCENE_CAS") ? atoi(getenv("SAVAGEWIND_SCENE_CAS")) : 1;
    bool mvec = mvec_enabled();
    bool dlss = g_settings.upscaler == UPSCALE_DLSS && vk_dlss_available();
    bool fsr = fsr_on && g_settings.upscaler == UPSCALE_FSR1;
    bool cas = cas_on && g_settings.sharpening > 0;
    static uint64_t grade_frame = ~0ull;
    static bool grade;
    if (grade_frame != renderer_present_count()) {
        grade_frame = renderer_present_count();
        grade = vk_exec_grade_wanted(NULL, NULL);
    }
    bool fsr3 = fsr_on && rt_fsr3() && mvec_enabled();
    if (!fsr3 && !dlss) rt_jitter_set_phases(8);
    if (!dlss && !fsr && !cas && !fsr3 && !grade && !mvec) return false;
    // (the old functions would now call scene_composite; the new gate runs the same heavy body)
    return true;
}

static bool old_fg(void) {
    return g_fg_want || (g_settings.upscaler == UPSCALE_DLSS && vk_dlss_available());
}

static void setup(bool post_on) {
    memset(&g_settings, 0, sizeof g_settings);
    g_settings.upscaler = post_on ? UPSCALE_FSR1 : UPSCALE_OFF;
    g_settings.antialias = post_on ? AA_TAA : AA_OFF;
    g_settings.sharpening = post_on ? 5 : 0;
    g_settings.color_contrast = post_on ? 15 : 0;
    mvec_on = fsr3_on = dlss_on = false;
    grade_on = post_on;
    g_fg_want = 0;
    jitter_phases = 0;
    vk_exec_post_frame = ~0ull;
    vk_exec_post_copy = false;
}

static double run(bool old, int draws, int frames) {
    uint64_t t0 = now_ns();
    for (int f = 0; f < frames; f++) {
        frame++;
        if (!old) vk_exec_post_update();  // (exec_list does this once per command list)
        for (int d = 0; d < draws; d++) {
            if (old) {
                if (old_hook()) heavy_calls++;
                if (old_fg()) heavy_calls++;
            } else {
                scene_post(NULL, NULL);
                fg_draw(NULL, NULL);
                fg_before_draw();
            }
            sink += jitter_phases;
        }
    }
    return (now_ns() - t0) / (double)(frames * draws);
}

static void compare(const char *name, bool post_on, int draws) {
    setup(post_on);
    heavy_calls = copy_calls = 0;
    double old = run(true, draws, 20);
    setup(post_on);
    heavy_calls = copy_calls = 0;
    double new_ = run(false, draws, 20);
    printf("  %-22s %5d draws  old %7.2f ns/draw  new %6.2f ns/draw  %6.1f%%   old %6.3f ms/frame  new %6.3f ms/frame\n",
           name, draws, old, new_, (new_ / old - 1) * 100, old * draws / 1e6, new_ * draws / 1e6);
}

int main(int argc, char **argv) {
    // The flags answer the same and the copy-pending hand-off reaches the copy.
    setup(false);
    frame++;
    vk_exec_post_update();
    assert(!vk_exec_post_on && !vk_exec_post_fg_on);
    vk_exec_post_set_copy_pending(true);
    fg_before_draw();
    assert(copy_calls == 1);

    if (!(argc > 1 && !strcmp(argv[1], "--bench"))) {
        puts("test_vk_exec_post: ok");
        return 0;
    }

    printf("exec_post_hook (per-draw feature gate, realistic draw counts from issues #332/#589)\n");
    compare("post off", false, 3663);
    compare("post off", false, 7382);
    compare("post on (gate)", true, 3663);
    return 0;
}
