// Bindless textures (maxwell/shader.h, ShaderInfo.uses_bindless): the handles a draw's vertices carry, for the
// renderers (Metal: renderer.m; Vulkan: vulkan/exec.c) to bind in the program's bindless slots.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "maxwell/shader.h"
#include "nvn.h"

// The vertex stream the handles come from: its data (guest memory), size (0: unknown), stride and instance divisor,
// and the handle's byte offset in a vertex (the attribute's offset plus 4 per component).
typedef struct { const uint8_t *data; uint64_t size; uint32_t stride, div, off; } BlStream;

// The distinct non-zero handles of the draw r (indexed or not) into out (0 = none), at most BINDLESS_SLOTS; returns
// how many did not fit
int bindless_gather(const CmdRec *r, bool indexed, const BlStream *s, uint32_t out[BINDLESS_SLOTS]);
