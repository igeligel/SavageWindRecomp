// How many bytes of a vertex stream a draw can read. exec.c turns the largest index (or the instance count) and the
// stream's stride into the copy bound.
#ifndef SW_EXEC_HELPERS_VERTEX_RANGE_H
#define SW_EXEC_HELPERS_VERTEX_RANGE_H

#include <stdint.h>

// stride 0: the constant zero stream; divisor: instanced stream; otherwise the largest index the draw reads. A known
// guest range bounds it.
static inline uint64_t vertex_need(uint32_t max_index, uint32_t inst, uint32_t base_inst, uint64_t stride,
                                   uint32_t divisor, uint64_t size) {
    uint64_t need;
    if (!stride) need = 256;
    else if (divisor) need = ((uint64_t)base_inst + inst + divisor - 1) / divisor * stride + 256;
    else need = (uint64_t)max_index * stride + 256;
    if (size && need > size) need = size;
    return need;
}

#endif
