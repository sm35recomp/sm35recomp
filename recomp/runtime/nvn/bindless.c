// Bindless textures: the handles of a draw's vertices (runtime/nvn/bindless.h)
#include "bindless.h"

#include <string.h>

int bindless_gather(const CmdRec *r, bool indexed, const BlStream *s, uint32_t out[BINDLESS_SLOTS]) {
    memset(out, 0, sizeof(uint32_t) * BINDLESS_SLOTS);
    if (!s->data) return 0;
    // draw arguments (renderers' draw): non-indexed mode, first, count, base instance, instances; indexed mode,
    // index type, count, index address, base vertex, base instance, instances
    uint32_t count = (uint32_t)r->x[2], nh = 0;
    int overflow = 0;
    uint32_t inst = indexed ? (uint32_t)r->x[6] : (uint32_t)r->x[4], base_inst = indexed ? (uint32_t)r->x[5] : (uint32_t)r->x[3];
    if (!inst) inst = 1;
    uint32_t total = !s->stride ? 1 : s->div ? inst : count;
    if (total > 65536) total = 65536;
    for (uint32_t i = 0; i < total; i++) {
        uint64_t id;
        if (!s->stride) id = 0;
        else if (s->div) id = (base_inst + i) / s->div;
        else if (!indexed) id = (uint32_t)r->x[1] + i;
        else {
            uint32_t itype = (uint32_t)r->x[1];
            const uint8_t *ip = (const uint8_t *)(uintptr_t)r->x[3];
            int32_t v = itype == 0 ? ip[i] : itype == 1 ? ((const uint16_t *)ip)[i] : (int32_t)((const uint32_t *)ip)[i];
            id = (uint32_t)(v + (int32_t)r->x[4]);
        }
        uint64_t at = id * s->stride + s->off;
        if (s->size && at + 4 > s->size) continue;
        uint32_t h;
        memcpy(&h, s->data + at, 4);
        if (!h) continue;
        uint32_t k = 0;
        while (k < nh && out[k] != h) k++;
        if (k < nh) continue;
        if (nh < BINDLESS_SLOTS) out[nh++] = h;
        else overflow++;
    }
    return overflow;
}
