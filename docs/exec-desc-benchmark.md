# Vulkan descriptor path: split and benchmark

`runtime/nvn/vulkan/exec.c` is being split into focused modules. This step moves the per-draw descriptor path into
`runtime/nvn/vulkan/exec_desc.c`:

- `vk_exec_stage_set` (the old `stage_set`)
- `cbuf_info` and `ubo_span`
- `set_finish` and the shared descriptor-set cache
- dynamic uniform-buffer offsets, the rscale table and the tint buffer

`exec.c` keeps the submission pools (`vk_alloc_set`), the command-list interpreter and the patch helpers it owns.

## Benchmark

`tests/test_vk_exec_desc.c` measures the real descriptor code with stubs for the Vulkan device, submission ring and
guest memory. Build and run it with:

```sh
make test-vk-exec-desc   # correctness
make bench-vk-exec-desc  # microbenchmarks + realistic draw streams
```

`SAVAGEWIND_VK_NO_DESC_CACHE=1` disables the new driver-block and descriptor-probe caches, for an A/B run on the same
binary.

The realistic streams use the draw counts and set reuse of the logs attached to issues #332 and #589 (1,963
`[vk] t=` lines): a median of **3,663 draws/frame**, **2.01 sets/draw** and **89% shared sets** (mean 87.6%, range
about 60-94%). The set contents are generated from a hot/cold material distribution; the logs do not contain
descriptor contents, and no game files are used.

Method: Debian 13 in Docker, arm64, clang `-O2`. Medians of 3 interleaved A/B runs. The times cover the descriptor path
only, not whole-frame rendering.

## Results

### Realistic draw streams

| Workload | Before | After | Change |
|---|---:|---:|---:|
| median open world, 3,663 draws | 636.8 ns/draw · 2.33 ms/frame | 485.8 ns/draw · 1.78 ms/frame | **-23.7%**, -0.55 ms/frame |
| heavy open world, 7,382 draws | 625.6 ns/draw · 4.62 ms/frame | 482.3 ns/draw · 3.56 ms/frame | **-22.9%**, -1.06 ms/frame |
| low-reuse, 3,663 draws | 631.7 ns/draw · 2.31 ms/frame | 532.2 ns/draw · 1.95 ms/frame | **-15.8%**, -0.36 ms/frame |

Measured set reuse in those streams was 89.8%, 90.0% and 79.7%.

### Microbenchmarks

| Path | Before | After | Change |
|---|---:|---:|---:|
| `cbuf_info` driver cbuf | 61.4 ns | 42.8 ns | -30.4% |
| `set_finish` cached set | 130.6 ns | 39.4 ns | -69.9% |
| `set_finish` new set | 133.9 ns | 146.4 ns | +9.3% |
| both sets/draw | 690.9 ns | 466.8 ns | -32.4% |

## What changed

- **Driver-constant cache**: `cbuf_info(k=0)` keeps the assembled driver block per stage/submission and rebuilds it
  only when its storage-buffer ranges (or the compute group size) change.
- **Descriptor probe cache**: a 4-way, 32-set probe cache indexed by a cheap sampled tag. It is populated after the
  existing hash table has seen a set, so repeated materials skip both hashes and compare the flat key directly.
  One-off sets pay only the probe; the new-set path is ~9% slower.
- **Inlined no-op**: `vk_exec_gmem_stage_prepare` is a static inline no-op outside `_WIN32`, removing a per-set
  cross-TU call.

The realistic run showed the first 2-way probe cache thrashing on the ~64-material + 8-program working set, so it is
now 4-way.

## Notes

- The benchmark stubs the device; `vk_alloc_set` and descriptor updates are not measured at their real driver cost.
  The realistic stream caches guest copies per submission, as `exec.c` does.
- The new-set cost is higher than before. It pays off when more than about 10% of sets repeat, which the issue logs
  show is the common case.
- `make runtime-objs` and `make test-vk-exec-desc` pass. No gameplay run or game data was used.
