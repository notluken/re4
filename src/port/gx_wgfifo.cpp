// See include/gx.h's TARGET_PC WGPipe/GXWGFifo comments: real GX FIFO backing for game code that
// writes straight to `GXWGFifo->field` instead of going through a GXParam1xx()/GX*() wrapper
// (src/game/mes.cpp's RomFont::draw(), src/game/dbmodule.cpp -- vendor code, cannot change).
#ifndef TARGET_PC
#error "src/port/gx_wgfifo.cpp is host-only (TARGET_PC)"
#endif

#include "gx.h"

#include <cstdio>
#include <cstdlib>

// The one live instance every `GXWGFifo->field = v` call site writes through.
WGPipe GXWGFifo[1];

// f64 has no real FIFO primitive on either real hardware or Aurora (the FIFO is 32-bit-word
// oriented) and nothing in this tree writes one (confirmed by grep, include/gx.h's own comment) --
// abort loudly instead of silently doing the wrong thing if that ever changes.
void WGPipe::F64Proxy::operator=(::f64) const
{
    std::fprintf(stderr, "GXWGFifo->f64 write: no real FIFO primitive exists for this (include/gx.h)\n");
    std::abort();
}
