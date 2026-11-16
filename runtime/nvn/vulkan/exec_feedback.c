// The attachment set a draw can sample while writing it, and the two write checks resolve_feedback makes per draw.
// Built once per draw: is_attachment then tests a small precomputed set (with a 64-bit bloom filter for the common
// "not an attachment" case) instead of comparing the texture against the depth and every color target each time.
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "exec_feedback.h"

FeedbackAttach feedback_attach;

static bool by_object(void) {
    static int on = -1;
    if (on < 0) on = getenv("SAVAGEWIND_VK_SNAP_BY_OBJECT") ? atoi(getenv("SAVAGEWIND_VK_SNAP_BY_OBJECT")) != 0 : !g_vk.is_mali;
    return on;
}

bool feedback_same_image(NvnTexture *a, NvnTexture *b) {
    return a == b || (!by_object() && a && b && a->backend && a->backend == b->backend);
}

void feedback_attachment_begin(NvnTexture *depth, NvnTexture *const color[8], int ncolor, uint8_t det_draw,
                               bool ds0, bool ds1, bool ds3, uint32_t ds5, uint32_t ds6, uint32_t ds7,
                               const uint32_t stencil_mask[2]) {
    feedback_attach.n = 0;
    feedback_attach.bloom = 0;
    feedback_attach.by_obj = by_object();
    if (depth) {
        feedback_attach.obj[feedback_attach.n] = depth;
        feedback_attach.be[feedback_attach.n++] = depth->backend;
        feedback_attach.bloom |= feedback_key(depth, feedback_attach.by_obj);
    }
    for (int i = 0; i < ncolor && i < 8; i++) {
        NvnTexture *t = color[i];
        if (!t || (det_draw & (1u << i))) continue;
        feedback_attach.obj[feedback_attach.n] = t;
        feedback_attach.be[feedback_attach.n++] = t->backend;
        feedback_attach.bloom |= feedback_key(t, feedback_attach.by_obj);
    }
    if (ds0 && ds1) feedback_attach.depth_ro = false;
    else if (!ds3) feedback_attach.depth_ro = true;
    else {
        uint32_t ops = ds5 | ds6 << 8 | ds7 << 16;
        feedback_attach.depth_ro = true;
        for (int f = 0; f < 2 && feedback_attach.depth_ro; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            if (!(stencil_mask[f] & 0xFF)) continue;
            for (int k = 0; k < 3; k++) {
                uint32_t v = (o >> (4 * k)) & 0xF;
                if (v >= 2 && v <= 8) { feedback_attach.depth_ro = false; break; }
            }
        }
    }
}

bool feedback_draw_writes_color(uint32_t out_mask, uint32_t channel_mask, int i) {
    return ((out_mask >> (i * 4)) & 0xF) && ((channel_mask >> (i * 4)) & 0xF);
}

bool feedback_depth_read_only(void) {
    return feedback_attach.depth_ro;
}
