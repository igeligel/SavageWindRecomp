// The once-a-frame feature flags behind exec.c's scene_post/fg_draw/fg_before_draw gates. On the common path (no
// upscaler, TAA, sharpening, grading or frame generation) a draw now tests one bool instead of
// mvec_enabled/dlss_wanted/rt_fsr3/settings and the jitter reset.
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../settings.h"
#include "../output_scale.h"
#include "dlss.h"
#include "exec_post_hook.h"
#include "mvec.h"

extern volatile int g_fg_want;

uint64_t vk_exec_post_frame;
bool vk_exec_post_on, vk_exec_post_fg_on, vk_exec_post_copy;
bool vk_exec_post_dlss, vk_exec_post_fsr1, vk_exec_post_cas, vk_exec_post_fsr3, vk_exec_post_grade_on;
VkImageView vk_exec_post_lut;
float vk_exec_post_gpc[16];

static int fsr_on = -1, cas_on = -1;

void vk_exec_post_update(void) {
    vk_exec_post_frame = renderer_present_count();
    if (fsr_on < 0) fsr_on = getenv("SAVAGEWIND_SCENE_FSR") ? atoi(getenv("SAVAGEWIND_SCENE_FSR")) : 1;
    if (cas_on < 0) cas_on = getenv("SAVAGEWIND_SCENE_CAS") ? atoi(getenv("SAVAGEWIND_SCENE_CAS")) : 1;
    bool mvec = mvec_enabled();
    vk_exec_post_dlss = g_settings.upscaler == UPSCALE_DLSS && vk_dlss_available();
    vk_exec_post_fsr1 = fsr_on && g_settings.upscaler == UPSCALE_FSR1;
    vk_exec_post_cas = cas_on && g_settings.sharpening > 0;
    vk_exec_post_fsr3 = fsr_on && rt_fsr3() && mvec;
    vk_exec_post_grade_on = vk_exec_grade_wanted(&vk_exec_post_lut, vk_exec_post_gpc);
    vk_exec_post_on = vk_exec_post_dlss || vk_exec_post_fsr1 || vk_exec_post_cas || vk_exec_post_fsr3 ||
                      vk_exec_post_grade_on || mvec;
    vk_exec_post_fg_on = g_fg_want || vk_exec_post_dlss;
    if (!vk_exec_post_fsr3 && !vk_exec_post_dlss) rt_jitter_set_phases(8);  // (FSR 3 / DLSS set their own)
    if (!vk_exec_post_on) vk_exec_scene_post_off();  // (a frame after the effects were on: drop its scene_up)
}
