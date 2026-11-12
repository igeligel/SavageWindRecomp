// Tests and benchmark for runtime/nvn/vulkan/exec_desc.c: the constant-buffer/descriptor-set path a draw runs twice a
// frame (exec.c calls vk_exec_stage_set). No Vulkan device or game data: the device-facing helpers are stubs, so this
// measures the descriptor assembly, dedupe/copy and set hashing/lookup that run on the CPU.
//
//   make test-vk-exec-desc    checks the answers
//   make bench-vk-exec-desc   prints ns/op for each function and a realistic draw stream
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../runtime/nvn/vulkan/exec_desc.c"

static volatile uint64_t sink;

// ---- stubs for the parts that need a device, the submission ring or the game's memory
VkG g_vk;
ExecState st;
float tex_sc[2][64][2];
uint32_t tex_wh[2][64][2];
uint64_t setc_gen = 1;
uint64_t stat_sets, stat_sets_reused;
bool maxwell_gmem_bda;

static uint8_t guest_mem[1 << 16];
static uint8_t trans_mem[1 << 20];

// The realistic stream below uses the draw counts and set reuse of the logs attached to issues #332/#589
// (median 3663 draws/frame, 2.01 sets/draw, 89% shared sets). The contents are generated: a small hot set of
// materials/programs, like the open world's repeated materials, with a tail of one-off ones.
#define REAL_MAT 512
#define REAL_PROG 16
static uint64_t mat_tex[REAL_MAT][16];
static float mat_tint[REAL_MAT][8];
static float mat_scale[REAL_MAT];
static uint64_t prog_buf[REAL_PROG][4];
static int bench_material, bench_program;
static uint64_t bench_seq = 1;
static int bench_guest_cache;

VkSpan vk_transient(uint64_t size, uint64_t align) {
    static uint64_t used;
    if (align < 16) align = 16;
    uint64_t off = (used + align - 1) & ~(align - 1);
    if (off + size > sizeof trans_mem) off = 0;
    used = off + size;
    return (VkSpan){(VkBuffer)(uintptr_t)0x1000, off, trans_mem + off};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

VkSpan guest_copy(uint64_t addr, uint64_t size) {
    static struct { uint64_t seq, addr, size; VkSpan span; } cache[256];
    if (bench_guest_cache) {
        uint32_t i = (uint32_t)(((addr >> 4) ^ size) & 255);
        if (cache[i].seq == bench_seq && cache[i].addr == addr && cache[i].size == size) return cache[i].span;
        VkSpan hit = vk_transient(size ? size : 16, 256);
        if (size) memcpy(hit.ptr, guest_mem + ((addr - GUEST_LO) & (sizeof guest_mem - 1)), size);
        hit.off = addr - GUEST_LO;
        cache[i] = (__typeof__(cache[0])){bench_seq, addr, size, hit};
        return hit;
    }
    VkSpan s = vk_transient(size ? size : 16, 256);
    if (size) memcpy(s.ptr, guest_mem + ((addr - GUEST_LO) & (sizeof guest_mem - 1)), size);
    s.off = addr - GUEST_LO;
    return s;
}

VkSpan copy_dedupe_in(int slot, const void *data, uint64_t size, uint64_t alloc, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient(alloc > size ? alloc : size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    return copy_dedupe_in(slot, data, size, 0, align);
}

bool ubo_direct(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    (void)addr; (void)size; (void)out;
    return false;
}

void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    (void)stage; (void)si;
    uint64_t h = mat_tex[bench_material][i];
    *out = (VkDescriptorImageInfo){(VkSampler)(uintptr_t)h, (VkImageView)(uintptr_t)(h ^ 0x1000),
                                   VK_IMAGE_LAYOUT_GENERAL};
    sc[0] = sc[1] = mat_scale[bench_material];
    wh[0] = wh[1] = 1024;
}

VkDescriptorBufferInfo gmem_info(void) { return (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x200, 0, 1 << 20}; }
VkDescriptorBufferInfo gmem_info_hi(void) { return (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x201, 0, 1 << 20}; }
VkDescriptorBufferInfo gsnap_info(int stage) { (void)stage; return (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x202, 0, 1 << 16}; }
VkDescriptorBufferInfo mvec_history_info(void) { return (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)0x203, 0, 1 << 16}; }

bool vk_exec_cbuf_patch(int stage, int k, uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    (void)stage; (void)k; (void)addr; (void)size; (void)out;
    return false;
}

#ifdef _WIN32
void vk_exec_gmem_stage_prepare(int stage, ShaderInfo *si) { (void)stage; (void)si; }
#endif

uint64_t vk_pending_seq(void) { return bench_seq; }

static uint64_t set_alloc;
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    (void)layout;
    return (VkDescriptorSet)(uintptr_t)++set_alloc;
}

void rec_UpdateDescriptorSets(uint32_t n, const VkWriteDescriptorSet *w) { (void)n; (void)w; }

VkImageView vk_dummy_view(int textype) { (void)textype; return (VkImageView)(uintptr_t)0x300; }
VkSampler vk_default_sampler(bool linear) { (void)linear; return (VkSampler)(uintptr_t)0x301; }

static double now_ns(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

typedef void (*bench_fn)(void);

static double bench(const char *name, bench_fn fn, int batch, const char *data) {
    for (int i = 0; i < 10; i++) fn();
    double t0 = now_ns();
    long reps = 0;
    do {
        fn();
        reps++;
    } while (now_ns() - t0 < 50e6 && reps < 10000000);
    double ns = (now_ns() - t0) / ((double)reps * batch);
    printf("  %-34s %-18s %9.2f ns   %7.2f M/s\n", name, data, ns, ns > 0 ? 1000.0 / ns : 0.0);
    return ns;
}

// ---- the draw's bindings: four constant buffers, one of them dynamic, and the fragment set's 16 textures
static ShaderInfo si_vertex, si_fragment, si_cbuf;
static NvnProgram program;

#define VBINDS 6
static VkBindingInfo vbinds[VBINDS] = {
    {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {0, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {0, 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
    {0, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
    {0, 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
};

#define FBINDS (6 + 16)
static VkBindingInfo fbinds[FBINDS];

static VkDescriptorSetLayout vlayout = (VkDescriptorSetLayout)(uintptr_t)0x400;
static VkDescriptorSetLayout flayout = (VkDescriptorSetLayout)(uintptr_t)0x401;
static uint32_t fanisotropy;

static void setup(void) {
    memset(&si_vertex, 0, sizeof si_vertex);
    memset(&si_fragment, 0, sizeof si_fragment);
    memset(&si_cbuf, 0, sizeof si_cbuf);
    memset(&st, 0, sizeof st);
    si_vertex.cbuf_size[0] = 0x500;
    si_vertex.cbuf_size[1] = 0x100;
    si_vertex.cbuf_size[3] = 256;
    si_vertex.cbuf_size[4] = 256;
    si_vertex.cbuf_size[5] = 256;
    si_fragment.cbuf_size[0] = 0x500;
    si_fragment.cbuf_size[1] = 0x100;
    si_fragment.cbuf_size[3] = 256;
    si_fragment.cbuf_size[4] = 256;
    si_fragment.ntex = 16;
    si_fragment.uses_rscale = true;
    si_fragment.uses_tint = true;
    si_cbuf = si_vertex;
    si_cbuf.cbuf_dynamic |= 1u << 3;  // (the 4 KB guest cbuf case below)
    si_vertex.cbuf_dynamic = (1u << 4) | (1u << 5);
    si_fragment.cbuf_dynamic = (1u << 4);
    program.stages[0].code_addr = GUEST_LO + 0x100;
    program.stages[0].const_offset = 0;
    program.stages[0].const_size = 0x100;
    program.stages[1].code_addr = GUEST_LO + 0x200;
    program.stages[1].const_offset = 0;
    program.stages[1].const_size = 0x100;
    st.program = &program;
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 18; i++) {
            st.ubuf[s][i].addr = GUEST_LO + 0x1000 * (i + 1);
            st.ubuf[s][i].size = 0x1000;
        }
    for (int i = 0; i < 16; i++) {
        fbinds[6 + i] = (VkBindingInfo){1, (uint32_t)(32 + i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        tex_sc[1][i][0] = tex_sc[1][i][1] = 1;
        tex_wh[1][i][0] = tex_wh[1][i][1] = 1024;
    }
    fbinds[0] = (VkBindingInfo){1, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    fbinds[1] = (VkBindingInfo){1, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    fbinds[2] = (VkBindingInfo){1, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    fbinds[3] = (VkBindingInfo){1, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC};
    fbinds[4] = (VkBindingInfo){1, 18, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    fbinds[5] = (VkBindingInfo){1, 19, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    for (int i = 0; i < 2; i++)
        for (int t = 0; t < 64; t++) {
            tex_sc[i][t][0] = tex_sc[i][t][1] = 1;
            tex_wh[i][t][0] = tex_wh[i][t][1] = 1024;
        }
    for (int m = 0; m < REAL_MAT; m++) {
        for (int t = 0; t < 16; t++) mat_tex[m][t] = 1 + (uint64_t)((m * 17 + t * 7) & 191);
        for (int k = 0; k < 8; k++) mat_tint[m][k] = (float)(((m >> (k & 3)) & 3) * 0.25f);
        mat_scale[m] = (m & 3) == 0 ? 1.5f : (m & 7) == 1 ? 1.25f : 1.0f;
    }
    for (int p = 0; p < REAL_PROG; p++)
        for (int k = 0; k < 4; k++) prog_buf[p][k] = GUEST_LO + 0x20000 + (uint64_t)(p * 0x1000 + k * 0x800);
    bench_material = bench_program = 0;
}

// ---- set_finish's writes: 4 buffers + 16 textures, as the fragment stage
#define NW 20
static VkWriteDescriptorSet w[NW];
static VkDescriptorBufferInfo wbi[4];
static VkDescriptorImageInfo wii[16];

static void setup_writes(void) {
    for (int i = 0; i < 4; i++) {
        wbi[i] = (VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)(0x500 + i), (uint64_t)i * 0x1000, 0x1000};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                      .pBufferInfo = &wbi[i]};
    }
    for (int i = 0; i < 16; i++) {
        wii[i] = (VkDescriptorImageInfo){(VkSampler)(uintptr_t)(i + 1), (VkImageView)(uintptr_t)(i + 0x100),
                                         VK_IMAGE_LAYOUT_GENERAL};
        w[4 + i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)(32 + i),
                                          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                          .pImageInfo = &wii[i]};
    }
}

// Each runner repeats the call: the timer's indirect call and loop overhead then count once per batch, not once per
// op. The batch is the count below, so the printed ns is still one call.
#define REP_UBO 64
#define REP_CBUF 32
#define REP_SET 16
#define REP_BOTH 8

static void run_ubo_span(void) {
    static VkSpan s = {(VkBuffer)(uintptr_t)0x600, 0x100, NULL};
    uint64_t acc = 0;
    for (int i = 0; i < REP_UBO; i++) acc += ubo_span(s, 0x4000).range;
    sink += acc;
}

static void run_cbuf_driver(void) {
    uint64_t acc = 0;
    for (int i = 0; i < REP_CBUF; i++) acc += cbuf_info(0, &si_vertex, 0, 0, 0, 0).range;
    sink += acc;
}

static void run_cbuf_code(void) {
    uint64_t acc = 0;
    for (int i = 0; i < REP_CBUF; i++) acc += cbuf_info(0, &si_vertex, 1, 0, 0, 0).offset;
    sink += acc;
}

static void run_cbuf_guest(void) {
    uint64_t acc = 0;
    for (int i = 0; i < REP_CBUF; i++) acc += cbuf_info(0, &si_cbuf, 3, 0, 0, 0).offset;
    sink += acc;
}

static void run_set_hit(void) {
    uintptr_t acc = 0;
    for (int i = 0; i < REP_SET; i++) acc ^= (uintptr_t)set_finish(vlayout, w, NW);
    sink ^= acc;
}

static void run_set_miss(void) {
    uintptr_t acc = 0;
    for (int i = 0; i < REP_SET; i++) {
        wbi[0].offset += 0x100;
        acc ^= (uintptr_t)set_finish(vlayout, w, NW);
    }
    sink ^= acc;
}

static uint32_t dynbuf[64];
static int ndyn;

static void run_stage_vertex(void) {
    uintptr_t acc = 0;
    for (int i = 0; i < REP_SET; i++) {
        ndyn = 0;
        acc ^= (uintptr_t)vk_exec_stage_set(vbinds, VBINDS, vlayout, 0, 0, &si_vertex, NULL, NULL, 0, 0, 0, NULL, dynbuf, &ndyn);
    }
    sink ^= acc;
}

static void run_stage_fragment(void) {
    float tint[8] = {0.25f, 0.5f, 0.75f, 1, 0, 0, 0, 0};
    uintptr_t acc = 0;
    for (int i = 0; i < REP_SET; i++) {
        ndyn = 0;
        acc ^= (uintptr_t)vk_exec_stage_set(fbinds, FBINDS, flayout, 1, 1, &si_fragment, NULL, tint, 0, 0, 0, NULL, dynbuf, &ndyn);
    }
    sink ^= acc;
}

static void run_stage_both(void) {
    uintptr_t acc = 0;
    float tint[8] = {0.25f, 0.5f, 0.75f, 1, 0, 0, 0, 0};
    for (int i = 0; i < REP_BOTH; i++) {
        ndyn = 0;
        acc ^= (uintptr_t)vk_exec_stage_set(vbinds, VBINDS, vlayout, 0, 0, &si_vertex, NULL, NULL, 0, 0, 0, NULL, dynbuf, &ndyn);
        ndyn = 0;
        acc ^= (uintptr_t)vk_exec_stage_set(fbinds, FBINDS, flayout, 1, 1, &si_fragment, NULL, tint, 0, 0, 0, NULL, dynbuf, &ndyn);
    }
    sink ^= acc;
}

static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static int last_material, last_program;

// The open world draws a material's objects in runs (the log's ~89% shared sets). Keep the previous material most of
// the time, then move to a hot one, with a tail of one-off materials.
static int pick_material(int reuse_pct) {
    uint32_t r = rnd() % 100;
    if ((int)r < reuse_pct) return last_material;
    if ((int)r < reuse_pct + (100 - reuse_pct) / 2) return last_material = (int)(rnd() % 64);
    return last_material = (int)(rnd() % REAL_MAT);
}

static int pick_program(void) {
    uint32_t r = rnd() % 100;
    if (r < 90) return last_program;
    return last_program = (int)(rnd() % (r < 95 ? 8 : REAL_PROG));
}

static void set_draw_state(int m, int p) {
    bench_material = m;
    bench_program = p;
    st.ubuf[0][0] = (Range){prog_buf[p][0], 0x1000};
    st.ubuf[0][1] = (Range){prog_buf[p][1], 0x1000};
    st.ubuf[0][2] = (Range){prog_buf[p][2], 0x1000};
    st.ubuf[1][0] = (Range){GUEST_LO + 0x40000 + (uint64_t)(m % 64) * 0x1000, 0x1000};
    st.ubuf[1][1] = (Range){GUEST_LO + 0x80000 + (uint64_t)(m % 64) * 0x1000, 0x1000};
}

static void realistic_frame(int draws, int frames, int reuse_pct, const char *label) {
    rng_state = 0x12345678u;
    last_material = last_program = 0;
    bench_guest_cache = 1;
    uint64_t sets0 = stat_sets, reused0 = stat_sets_reused;
    double t0 = now_ns();
    for (int f = 0; f < frames; f++) {
        bench_seq++;
        for (int d = 0; d < draws; d++) {
            int m = pick_material(reuse_pct), p = pick_program();
            set_draw_state(m, p);
            ndyn = 0;
            sink ^= (uintptr_t)vk_exec_stage_set(vbinds, VBINDS, vlayout, 0, 0, &si_vertex, NULL, NULL, 0, 0, 0, NULL,
                                                 dynbuf, &ndyn);
            ndyn = 0;
            sink ^= (uintptr_t)vk_exec_stage_set(fbinds, FBINDS, flayout, 1, 1, &si_fragment, NULL, mat_tint[m], 0, 0, 0,
                                                 NULL, dynbuf, &ndyn);
        }
    }
    double ns = now_ns() - t0;
    uint64_t sets = stat_sets - sets0, shared = stat_sets_reused - reused0;
    printf("  %-30s %5d draws x %2d frames  %7.1f ns/draw  %6.2f ms/frame  sets %llu shared %llu (%.1f%%)\n",
           label, draws, frames, ns / (double)(frames * draws), ns / frames / 1e6,
           (unsigned long long)sets, (unsigned long long)shared,
           sets ? 100.0 * (double)shared / (double)sets : 0.0);
    bench_guest_cache = 0;
}

int main(int argc, char **argv) {
    setup();
    setup_writes();
    bool run_bench = argc > 1 && !strcmp(argv[1], "--bench");

    // The dynamic buffers come out by binding (4 before 5), whatever the order of the bindings.
    ndyn = 0;
    VkDescriptorSet ds = vk_exec_stage_set(vbinds, VBINDS, vlayout, 0, 0, &si_vertex, NULL, NULL, 0, 0, 0, NULL, dynbuf, &ndyn);
    assert(ds != VK_NULL_HANDLE);
    assert(ndyn == 2);
    assert(dynbuf[0] == 0x2000 && dynbuf[1] == 0x3000);  // cbuf 4: ubuf[1]; cbuf 5: ubuf[2]

    // Identical contents become the same set.
    setup_writes();
    uint64_t reused = stat_sets_reused;
    VkDescriptorSet a = set_finish(vlayout, w, NW);
    VkDescriptorSet b = set_finish(vlayout, w, NW);
    assert(a == b && stat_sets_reused == reused + 1);

    ndyn = 0;
    assert(vk_exec_stage_set(fbinds, FBINDS, flayout, 1, 1, &si_fragment, NULL, NULL, 0, 0, 0, NULL, dynbuf, &ndyn) != VK_NULL_HANDLE);
    assert(ndyn == 1);

    if (!run_bench) {
        puts("test_vk_exec_desc: ok");
        return 0;
    }

    printf("exec_desc (descriptor assembly without a device)\n");
    double span = bench("ubo_span", run_ubo_span, REP_UBO, "1 span");
    (void)span;
    double drv = bench("cbuf_info", run_cbuf_driver, REP_CBUF, "driver cbuf");
    double code = bench("cbuf_info", run_cbuf_code, REP_CBUF, "code cbuf 256 B");
    double guest = bench("cbuf_info", run_cbuf_guest, REP_CBUF, "guest cbuf 4K");
    double hit = bench("set_finish", run_set_hit, REP_SET, "cached set");
    double miss = bench("set_finish", run_set_miss, REP_SET, "new set");
    double vtx = bench("vk_exec_stage_set", run_stage_vertex, REP_SET, "vertex 4 cbuf");
    double frag = bench("vk_exec_stage_set", run_stage_fragment, REP_SET, "fragment 16 tex");
    double both = bench("vk_exec_stage_set", run_stage_both, REP_BOTH, "both sets/draw");
    printf("\nper draw (both sets): %.1f ns; at 5000 draws: %.2f ms/frame\n", both, both * 5000 / 1e6);
    printf("fragment set: %.1f ns; set cache hit: %.1f ns; miss: %.1f ns; cbuf driver: %.1f ns; code: %.1f ns; guest: %.1f ns\n",
           frag, hit, miss, drv, code, guest);

    printf("\nrealistic draw stream (issue #332/#589 logs: median 3663 draws/frame, 2.01 sets/draw, ~89%% shared)\n");
    realistic_frame(3663, 12, 80, "median open-world frame");
    realistic_frame(7382, 6, 80, "heavy open-world frame");
    realistic_frame(3663, 8, 60, "low-reuse frame");
    return 0;
}
