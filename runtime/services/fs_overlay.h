// The appended RomFS region's entry lookup (runtime/services/fs.c). The game's read offsets are arbitrary, so this is
// the question every read past the image asks: which replaced or added file holds it.
#pragma once
#include <stdint.h>

// A file replaced or added past the image: its virtual start, its real size, and either a host path or an in-memory
// blob. Entries are appended in start order and are 16-byte rounded, so they never overlap.
typedef struct { uint64_t start, size; char *path; int fd; const uint8_t *mem; } Overlay;

// Bytes an entry occupies in the virtual image (reads in the rounding padding are zero-filled by romfs_read).
static inline uint64_t ov_span(uint64_t size) { return (size + 0xF) & ~(uint64_t)0xF; }

// Index of the entry holding image offset o, or -1 (including the padding gaps between entries). ov[] is sorted by
// start, so the answer is the stored upper bound of o by start, one entry back, if o is still inside that entry's
// span. Inlined and without bsearch's comparison pointer: this is on the read hot path, and the image region (o before
// the first entry, most reads) returns at the guard.
static inline int ov_find(const Overlay *ov, int n, uint64_t o) {
    if (!n || o < ov[0].start) return -1;
    int lo = 0, hi = n;
    while (lo < hi) {  // upper bound: first entry starting past o
        int mid = (lo + hi) >> 1;
        if (ov[mid].start <= o) lo = mid + 1;
        else hi = mid;
    }
    int i = lo - 1;
    return o < ov[i].start + ov_span(ov[i].size) ? i : -1;
}
