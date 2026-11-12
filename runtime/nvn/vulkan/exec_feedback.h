// The per-draw attachment membership and write checks of the feedback path (exec_feedback.c). resolve_feedback in
// exec.c builds the draw's attachment set once, then asks whether each bound texture is one of them.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vk.h"

typedef struct {
    NvnTexture *obj[9];
    TexBackend *be[9];
    uint64_t bloom;
    int n;
    bool by_obj;
    bool depth_ro;
} FeedbackAttach;

extern FeedbackAttach feedback_attach;

static inline uint64_t feedback_key(NvnTexture *t, bool by_obj) {
    uintptr_t k = by_obj || !t->backend ? (uintptr_t)t : (uintptr_t)t->backend;
    return k ? 1ull << ((k >> 4) & 63) : 0;
}

static inline bool feedback_is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (!(feedback_attach.bloom & feedback_key(t, feedback_attach.by_obj))) return false;
    for (int i = 0; i < feedback_attach.n; i++)
        if (feedback_attach.by_obj ? feedback_attach.obj[i] == t
                                   : feedback_attach.be[i] ? feedback_attach.be[i] == t->backend
                                                           : feedback_attach.obj[i] == t)
            return true;
    return false;
}

void feedback_attachment_begin(NvnTexture *depth, NvnTexture *const color[8], int ncolor, uint8_t det_draw,
                               bool ds0, bool ds1, bool ds3, uint32_t ds5, uint32_t ds6, uint32_t ds7,
                               const uint32_t stencil_mask[2]);
bool feedback_same_image(NvnTexture *a, NvnTexture *b);
bool feedback_draw_writes_color(uint32_t out_mask, uint32_t channel_mask, int i);
bool feedback_depth_read_only(void);
