// Descriptor sets of the Vulkan command executor: the constant buffers a draw binds, the shared sets of identical
// contents, and the two stage sets a draw writes (exec.c calls vk_exec_stage_set).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "exec/internal.h"
#include "mvec.h"
#include "record.h"

VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// The driver's constant block (cbuf 0) is the same for every draw of a submission that binds the same storage
// buffers: keep the last one per stage and rebuild it only when one of those ranges (or the compute group size)
// changes. The span is valid while the submission is pending, like every transient copy.
typedef struct {
    uint64_t seq, size;
    uint32_t gx, gy, gz;
    Range sbuf[16], ubuf[16];
    VkDescriptorBufferInfo d;
} DrvCache;
static DrvCache drv_cache[6];

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        static int no_cache = -1;
        if (no_cache < 0) no_cache = getenv("SAVAGEWIND_VK_NO_DESC_CACHE") != NULL;
        uint32_t drv[0x800 / 4] = {0};
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        uint64_t seq = vk_pending_seq();
        DrvCache *c = &drv_cache[stage];
        if (!no_cache && c->seq == seq && c->size == size && !memcmp(c->sbuf, st.sbuf[stage], sizeof c->sbuf) &&
            (stage != 5 || (c->gx == gx && c->gy == gy && c->gz == gz && !memcmp(c->ubuf, st.ubuf[5], sizeof c->ubuf))))
            return c->d;
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        VkDescriptorBufferInfo d =
            ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
        if (!no_cache) {
            c->seq = seq;
            c->size = size;
            memcpy(c->sbuf, st.sbuf[stage], sizeof c->sbuf);
            if (stage == 5) {
                c->gx = gx; c->gy = gy; c->gz = gz;
                memcpy(c->ubuf, st.ubuf[5], sizeof c->ubuf);
            } else {
                c->gx = c->gy = c->gz = 0;
                memset(c->ubuf, 0, sizeof c->ubuf);
            }
            c->d = d;
        }
        return d;
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        VkDescriptorBufferInfo direct;
        if (ubo_direct(ss->code_addr + ss->const_offset, size, &direct)) return direct;  // (constants: never rewritten)
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    // An address outside the game's memory ended the game in the copy below after two hours of play (#640: 0x5000000000
    // on the render thread). Read as an unbound buffer, and logged: where the address comes from is still unknown.
    if (r.addr < GUEST_LO || r.addr >= GUEST_HI || size > GUEST_HI - r.addr) {
        static int nbad;
        if (nbad++ < 16)
            LOG("[vk] constant buffer %d of stage %d at %llx (%llu bytes) is outside the game's memory: read as zeros\n", k,
                stage, (unsigned long long)r.addr, (unsigned long long)size);
        return ubo_span(vk_transient_copy(zeros, size, 256), size);
    }
    VkDescriptorBufferInfo direct;
    if (vk_exec_cbuf_patch(stage, k, r.addr, size, &direct)) return direct;
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(r.addr, size), size);
}

// Descriptor sets of identical contents within one submission are one set (the open world draws many objects with
// the same material textures and constants): saves the allocation and the update. SAVAGEWIND_VK_NO_SET_CACHE=1: off.
// Entries are told apart by two independent 64-bit hashes of the contents (no stored key: comparing ~700-byte keys
// from a 3 MB table cost more than the hashing).
#define SETC_TAB 4096
typedef struct { uint64_t seq, hash, hash2; VkDescriptorSetLayout layout; VkDescriptorSet set; uint32_t n; } SetEnt;
static SetEnt *setc;

// The last few sets by a cheap probe of their contents: a repeated draw (the same material's textures and constants)
// then compares the flat key and returns the set without the two hashes. Only sets the hash table has already seen are
// put here, so a one-off set pays just the probe, not the key copy. Misses fall through to the hash table.
#define SETP_TAB 32
#define SETP_WAYS 4
typedef struct {
    uint64_t seq, tag;
    VkDescriptorSetLayout layout;
    uint32_t n;
    VkDescriptorSet set;
    uint64_t key[4 * MAX_BIND];
} SetProbe;
static SetProbe setp[SETP_TAB][SETP_WAYS];
static uint32_t setp_next[SETP_TAB];

static void set_key(const VkWriteDescriptorSet *w, int n, uint64_t *key) {
    for (int i = 0; i < n; i++) {
        uint64_t *v = &key[4 * i];
        v[0] = w[i].dstBinding | (uint64_t)w[i].descriptorType << 32;
        if (w[i].pImageInfo) {
            v[1] = (uint64_t)(uintptr_t)w[i].pImageInfo->sampler;
            v[2] = (uint64_t)(uintptr_t)w[i].pImageInfo->imageView;
            v[3] = w[i].pImageInfo->imageLayout;
        } else {
            v[1] = (uint64_t)(uintptr_t)w[i].pBufferInfo->buffer;
            v[2] = w[i].pBufferInfo->offset;
            v[3] = w[i].pBufferInfo->range;
        }
    }
}

static uint64_t set_tag(const VkWriteDescriptorSet *w, int n, VkDescriptorSetLayout layout) {
    uint64_t tag = (uint64_t)(uintptr_t)layout ^ (uint64_t)n;
    for (int i = 0; i < n; i++) {
        if (n > 4 && i >= 2 && i < n - 2) continue;  // (first 2 and last 2: a cheap sample of the contents)
        uint64_t v[4];
        v[0] = w[i].dstBinding | (uint64_t)w[i].descriptorType << 32;
        if (w[i].pImageInfo) {
            v[1] = (uint64_t)(uintptr_t)w[i].pImageInfo->sampler;
            v[2] = (uint64_t)(uintptr_t)w[i].pImageInfo->imageView;
            v[3] = w[i].pImageInfo->imageLayout;
        } else {
            v[1] = (uint64_t)(uintptr_t)w[i].pBufferInfo->buffer;
            v[2] = w[i].pBufferInfo->offset;
            v[3] = w[i].pBufferInfo->range;
        }
        for (int k = 0; k < 4; k++) tag = ((tag << 7) | (tag >> 57)) ^ v[k];
    }
    return tag;
}

static VkDescriptorSet set_finish(VkDescriptorSetLayout layout, VkWriteDescriptorSet *w, int n) {
    static int off = -1, no_desc = -1;
    if (off < 0) off = getenv("SAVAGEWIND_VK_NO_SET_CACHE") != NULL;
    if (no_desc < 0) no_desc = getenv("SAVAGEWIND_VK_NO_DESC_CACHE") != NULL;
    stat_sets++;
    SetEnt *e = NULL;
    SetProbe *pp = NULL;
    uint64_t seq = vk_pending_seq() + (setc_gen << 40), h = 0, h2 = 0, tag = 0;
    if (!off) {
        if (!no_desc) {
            tag = set_tag(w, n, layout);
            SetProbe *ways = setp[tag & (SETP_TAB - 1)];
            for (int way = 0; way < SETP_WAYS; way++) {
                pp = &ways[way];
                if (pp->seq == seq && pp->layout == layout && pp->n == (uint32_t)n && pp->tag == tag) {
                    uint64_t key[4 * MAX_BIND];
                    set_key(w, n, key);
                    if (!memcmp(pp->key, key, (size_t)n * 4 * sizeof key[0])) {
                        stat_sets_reused++;
                        return pp->set;
                    }
                }
            }
            pp = NULL;
        }
        h = (uint64_t)(uintptr_t)layout;
        h2 = h ^ 0x6A09E667F3BCC909ull;
        for (int i = 0; i < n; i++) {
            uint64_t v[4];
            v[0] = w[i].dstBinding | (uint64_t)w[i].descriptorType << 32;
            if (w[i].pImageInfo) {
                v[1] = (uint64_t)(uintptr_t)w[i].pImageInfo->sampler;
                v[2] = (uint64_t)(uintptr_t)w[i].pImageInfo->imageView;
                v[3] = w[i].pImageInfo->imageLayout;
            } else {
                v[1] = (uint64_t)(uintptr_t)w[i].pBufferInfo->buffer;
                v[2] = w[i].pBufferInfo->offset;
                v[3] = w[i].pBufferInfo->range;
            }
            for (int k = 0; k < 4; k++) {
                h = (h ^ v[k]) * 0x9E3779B97F4A7C15ull; h ^= h >> 29;
                h2 = (h2 ^ v[k]) * 0xC2B2AE3D27D4EB4Full; h2 ^= h2 >> 31;
            }
        }
        h ^= h >> 32;
        if (!setc) setc = calloc(SETC_TAB, sizeof *setc);
        e = &setc[(h ^ (h >> 31)) & (SETC_TAB - 1)];
        if (e->seq == seq && e->hash == h && e->hash2 == h2 && e->layout == layout && e->n == (uint32_t)n) {
            stat_sets_reused++;
            if (!no_desc) {
                uint32_t idx = tag & (SETP_TAB - 1);
                SetProbe *ways = setp[idx];
                pp = NULL;
                for (int way = 0; way < SETP_WAYS; way++)
                    if (ways[way].seq != seq) { pp = &ways[way]; break; }
                if (!pp) pp = &ways[setp_next[idx]++ & (SETP_WAYS - 1)];
                uint64_t key[4 * MAX_BIND];
                set_key(w, n, key);
                pp->seq = seq; pp->tag = tag; pp->layout = layout; pp->n = (uint32_t)n; pp->set = e->set;
                memcpy(pp->key, key, (size_t)n * 4 * sizeof key[0]);
            }
            return e->set;
        }
    }
    VkDescriptorSet ds = vk_alloc_set(layout);
    for (int i = 0; i < n; i++) w[i].dstSet = ds;
    if (n) rec_UpdateDescriptorSets((uint32_t)n, w);
    if (e) {
        e->seq = vk_pending_seq() + (setc_gen << 40);  // (the set lives in this submission's pools)
        e->hash = h; e->hash2 = h2; e->layout = layout; e->set = ds; e->n = (uint32_t)n;
    }
    return ds;
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
// dyn / ndyn: the offsets of its dynamic uniform buffers, by binding (ubo_dynamic_mark), appended.
VkDescriptorSet vk_exec_stage_set(const VkBindingInfo *binds, int nbinds, VkDescriptorSetLayout layout, uint32_t set,
                                  int stage, ShaderInfo *si, const float *rscale_row0, const float tint[8], uint32_t gx,
                                  uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii, uint32_t *dyn, int *ndyn) {
    vk_exec_gmem_stage_prepare(stage, si);
    struct { uint32_t binding, off; } dv[32];
    int nd = 0;
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    // The scale table: row 0, then one per texture (translate.c reads rscale[1 + slot], slot < ntex). Only those rows
    // are filled, compared and copied: the whole 65-row table, zeroed, compared and copied twice a draw (~8000 times a
    // frame in the open world), was a share of the translation thread's time. The span keeps the declared size.
    int rows = 1 + (si->ntex < 64 ? si->ntex : 64);
    float rs[65][4];
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    else rs[0][0] = rs[0][1] = 1, rs[0][2] = rs[0][3] = 0;
    for (int i = 0; i < nbinds; i++) {
        const VkBindingInfo *b = &binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = VK_NULL_HANDLE, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding == GMEM_BUFFER_INDEX && maxwell_gmem_bda) {
            bi[n] = gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && nd < 32) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            int k = nd++;  // (by binding: the order vkCmdBindDescriptorSets takes them in)
            while (k > 0 && dv[k - 1].binding > b->binding) { dv[k] = dv[k - 1]; k--; }
            dv[k].binding = b->binding;
            dv[k].off = (uint32_t)bi[n].offset;
            bi[n].offset = 0;
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = b->binding == MVEC_HISTORY_BINDING ? mvec_history_info()
                  : b->binding == GSNAP_BUFFER_INDEX   ? gsnap_info(stage)
                  : b->binding == GMEM2_BUFFER_INDEX   ? gmem_info_hi()
                                                       : gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < nbinds; i++) {
        const VkBindingInfo *b = &binds[i];
        if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || (b->binding != 18 && b->binding != 19)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = VK_NULL_HANDLE, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++)
                rs[1 + t][0] = tex_sc[sg][t][0], rs[1 + t][1] = tex_sc[sg][t][1], rs[1 + t][2] = rs[1 + t][3] = 0;
            bi[n] = ubo_span(copy_dedupe_in(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, (uint64_t)rows * 16, sizeof rs, 256), sizeof rs);
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    for (int i = 0; i < nd && dyn; i++) dyn[(*ndyn)++] = dv[i].off;
    return set_finish(layout, w, n);
}
