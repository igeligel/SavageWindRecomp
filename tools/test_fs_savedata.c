// The save hot path's savedata gate (runtime/services/fs_savedata.h): whether a host path is one of the game's save
// files, which runtime/services/fs.c now answers once in IFileSystem::OpenFile and keeps in the HostFile instead of
// running strstr on every read and write. During an autosave the game issues many small reads and writes to a handful
// of files, so this checks the cached answer against the old per-operation scan on autosave-shaped traces, and with
// --bench compares the two on realistic short and full paths.
//
// The trace and the open/operation split mirror file() cases 0 and 1: the flag is computed once per opened file, then
// every read or write only reads it. No game files: the names are the save folder's layout from tools/run.sh.
//
// Run: make test-fs-savedata   /   make bench-fs-savedata
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "../runtime/services/fs_savedata.h"

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

// ------------------------------------------------------------------ fixtures
// The save root and SD card, as the game opens them: only the save data's own files carry "savedata" in their path
// (save_0/savedata0.bin in tools/run.sh's scratch save); the slots, the screenshot, the tracker and the SD card don't.
static const char *const file_names[] = {
    "save/save_0/savedata0.bin",
    "save/save_0/savedata1.bin",
    "save/save_0/screenshot0.bin",
    "save/save_0/0/game_data.sav",
    "save/save_0/1/game_data.sav",
    "save/save_0/2/game_data.sav",
    "save/save_0/option.sav",
    "save/save_0/tracker/trackblock00.sav",
    "save/save_0/tracker/trackblock01.sav",
    "save/save_1/savedata0.bin",
    "sdcard/album/0/photo0.jpg",
    "sdcard/Pack/Bootup.pack",
};
static const bool file_savedata[] = {
    true, true, false, false, false, false, false, false, false, true, false, false,
};
enum { NFILES = (int)(sizeof file_names / sizeof *file_names) };

// A full path, the length a real save root has with the player's profile in it.
static const char *const full_prefix = "/home/player/.local/share/SavageWind/";

// The opened file's state, as HostFile carries it past open: the path and the savedata answer.
typedef struct { char path[1100]; bool is_savedata; } OpenFile;

// One read or write of an opened file (offset and size don't enter the gate).
typedef struct { uint32_t file, size; bool write; } Op;

static uint64_t rnd(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *s = x;
}

static void open_file(OpenFile *f, const char *prefix, const char *name) {
    snprintf(f->path, sizeof f->path, "%s%s", prefix, name);
    f->is_savedata = fs_savedata_path(f->path);  // (the one scan IFileSystem::OpenFile does)
}

static void build_trace(Op *ops, size_t n, uint64_t seed) {
    for (size_t i = 0; i < n; i++) {
        uint64_t r = rnd(&seed);
        ops[i].file = (uint32_t)(r % NFILES);
        ops[i].size = (uint32_t)((r >> 16) & 0xFFF);
        ops[i].write = (bool)((r >> 40) & 1);
    }
}

// ------------------------------------------------------------------ tests
static int failures;

static void check(bool ok, const char *what, const char *path) {
    if (ok) return;
    fprintf(stderr, "FAIL: %s%s%s\n", what, path ? ": " : "", path ? path : "");
    failures++;
}

static void test_predicate(void) {
    static const struct { const char *path; bool want; } cases[] = {
        {"", false},
        {"savedata", true},
        {"savedata0.bin", true},
        {"/savedata0.bin", true},
        {"save/save_0/savedata0.bin", true},
        {"xsavedatasuffix", true},
        {"savedata/inside/path", true},
        {"SAVEDATA0.BIN", false},  // (the old strstr was case-sensitive too)
        {"save_data0.bin", false},
        {"savedat", false},
        {"screenshot0.bin", false},
        {"save/save_0/0/game_data.sav", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        check(fs_savedata_path(cases[i].path) == cases[i].want, "path classification", cases[i].path);
}

static void check_files(const OpenFile *files, const char *what) {
    for (int i = 0; i < NFILES; i++)
        check(files[i].is_savedata == file_savedata[i], what, files[i].path);
    // The cached answer is the old per-operation answer, for every file.
    for (int i = 0; i < NFILES; i++)
        check(files[i].is_savedata == fs_savedata_path(files[i].path), "cached vs scan", files[i].path);
}

static void test_cached_matches_scan(const char *prefix, uint64_t seed) {
    OpenFile files[NFILES];
    for (int i = 0; i < NFILES; i++) open_file(&files[i], prefix, file_names[i]);
    check_files(files, "open flag");

    enum { READS_WRITES = 200000 };
    Op *ops = malloc(READS_WRITES * sizeof *ops);
    if (!ops) {
        fprintf(stderr, "out of memory for the trace\n");
        exit(1);
    }
    build_trace(ops, READS_WRITES, seed);
    for (size_t i = 0; i < READS_WRITES; i++) {
        const OpenFile *f = &files[ops[i].file];
        bool per_op = fs_savedata_path(f->path);  // (what cases 0 and 1 used to run)
        if (per_op != f->is_savedata) {
            char what[128];
            snprintf(what, sizeof what, "op %zu (%s)", i, ops[i].write ? "write" : "read");
            check(false, what, f->path);
            break;  // (one report is enough on a mismatch)
        }
    }
    free(ops);
}

static void run_tests(void) {
    test_predicate();
    test_cached_matches_scan("", 0x9E3779B97F4A7C15ull);
    test_cached_matches_scan(full_prefix, 0xD1B54A32D192ED03ull);
    if (failures) {
        fprintf(stderr, "fs savedata gate: %d failure(s)\n", failures);
        exit(1);
    }
    printf("fs savedata gate: all tests passed (%d files, 400000 reads/writes)\n", NFILES);
}

// ------------------------------------------------------------------ benchmarks
#ifdef _WIN32
static uint64_t now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
}
#else
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

// The two candidates, as the read and write cases run them: a strstr of the path per operation (what the file
// service did), or the flag the file kept from open (what it does now). Not inlined, so neither loop can hoist the
// answer out of it.
static NOINLINE uint64_t ops_scan(const OpenFile *files, const Op *ops, size_t n) {
    uint64_t hit = 0;
    for (size_t i = 0; i < n; i++) hit += fs_savedata_path(files[ops[i].file].path);
    return hit;
}

static NOINLINE uint64_t ops_cached(const OpenFile *files, const Op *ops, size_t n) {
    uint64_t hit = 0;
    for (size_t i = 0; i < n; i++) hit += files[ops[i].file].is_savedata;
    return hit;
}

enum { BENCH_OPS = 2000000, BENCH_REPS = 7 };

static void bench_set(const char *label, const char *prefix) {
    OpenFile files[NFILES];
    for (int i = 0; i < NFILES; i++) open_file(&files[i], prefix, file_names[i]);
    Op *ops = malloc(BENCH_OPS * sizeof *ops);
    if (!ops) {
        fprintf(stderr, "bench: out of memory\n");
        exit(1);
    }
    build_trace(ops, BENCH_OPS, 0x5EED ^ (uint64_t)prefix[0]);

    volatile uint64_t sink = 0;  // (volatile: the calls must run, not be optimized away as unused)
    uint64_t best_scan = UINT64_MAX, best_cached = UINT64_MAX;
    for (int rep = 0; rep < BENCH_REPS; rep++) {
        uint64_t t0 = now_ns();
        sink += ops_scan(files, ops, BENCH_OPS);
        uint64_t dt = now_ns() - t0;
        if (dt < best_scan) best_scan = dt;
    }
    for (int rep = 0; rep < BENCH_REPS; rep++) {
        uint64_t t0 = now_ns();
        sink += ops_cached(files, ops, BENCH_OPS);
        uint64_t dt = now_ns() - t0;
        if (dt < best_cached) best_cached = dt;
    }
    double scan = (double)best_scan / (double)BENCH_OPS, cached = (double)best_cached / (double)BENCH_OPS;
    printf("%s (%zu-char path)\n", label, strlen(files[0].path));
    printf("  strstr per op (old)  %6.2f ns/op  %7.1f Mops/s\n", scan, 1000.0 / scan);
    printf("  cached bool (new)    %6.2f ns/op  %7.1f Mops/s  %.1fx  (checksum %llu)\n", cached,
           1000.0 / cached, cached > 0 ? scan / cached : 0.0, (unsigned long long)sink);
    free(ops);
}

static void run_bench(void) {
    printf("save hot path gate benchmark (best of %d, %d reads/writes each, autosave-shaped trace over %d files)\n",
           BENCH_REPS, BENCH_OPS, NFILES);
    printf("(the cached side also pays one strstr per file at open: %d scans, amortized over the trace)\n", NFILES);
    bench_set("save root", "");
    bench_set("full player path", full_prefix);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        run_bench();
        return 0;
    }
    run_tests();
    return 0;
}
