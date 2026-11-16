// The per-draw post-processing/frame-generation hook (exec_post_hook.c). exec.c owns the heavy scene_post/fg_draw
// bodies and the scene composite detection; this module owns the once-a-frame feature flags and the cheap per-draw
// gate, so a draw with the effects off does one bool test instead of the settings/mvec/grade checks.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vk.h"

typedef struct Prog Prog;

uint64_t renderer_present_count(void);
extern uint64_t vk_exec_post_frame;

// Set once a frame by vk_exec_post_update (called from exec.c when the presented frame changes).
extern bool vk_exec_post_on;       // any scene post effect, or motion vectors to fill
extern bool vk_exec_post_fg_on;    // frame generation, or DLSS's motion vectors
extern bool vk_exec_post_copy;     // a HUD-less copy is pending after the composite
extern bool vk_exec_post_dlss, vk_exec_post_fsr1, vk_exec_post_cas, vk_exec_post_fsr3, vk_exec_post_grade_on;
extern VkImageView vk_exec_post_lut;
extern float vk_exec_post_gpc[16];

void vk_exec_post_update(void);

// exec.c: the heavy bodies and the scene_post off-path cleanup.
bool vk_exec_grade_wanted(VkImageView *lut, float pc[16]);
void vk_exec_scene_post_do(Prog *pr, CmdRec *r);
void vk_exec_scene_post_off(void);
void vk_exec_fg_draw_do(Prog *pr, CmdRec *r);
void vk_exec_fg_copy_hudless_do(void);

static inline void scene_post(Prog *pr, CmdRec *r) {
    if (!vk_exec_post_on) return;
    vk_exec_scene_post_do(pr, r);
}

static inline void fg_before_draw(void) {
    if (!vk_exec_post_copy) return;
    vk_exec_fg_copy_hudless_do();
}

static inline void fg_draw(Prog *pr, CmdRec *r) {
    if (!vk_exec_post_fg_on) return;
    vk_exec_fg_draw_do(pr, r);
}

static inline void vk_exec_post_set_copy_pending(bool pending) {
    vk_exec_post_copy = pending;
}
