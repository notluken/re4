#ifndef TPL_H
#define TPL_H

#include "types.h"
#ifdef TARGET_PC
#include "port/ptr32.h"
#include "port/be.h"
#endif

// TPL texture palette layout (charPipeline/texPalette.h without the SDK includes). File offsets
// are relocated to pointers by cTexSys::CalcTplAddr. The pointer fields below (docs/port-phase2.md,
// "the inventory") are Ptr32<T> under TARGET_PC, same reasoning as cModelData (include/model.h).
// The plain integer/float fields are big-endian on disc (Phase 3, docs/port-phase3.md) -- BE<T>
// under TARGET_PC swaps them on every read/write; `data` itself (the pixel/CLUT payload the
// pointer field addresses) is deliberately left untouched -- Aurora's own GX texture-conversion
// path (../aurora/lib/gfx/texture_convert.cpp) already byte-swaps texel/CLUT data internally,
// exactly as real GX hardware would consume it, so pre-swapping the payload here would double-swap.
struct CLUTHeader {
#ifdef TARGET_PC
    re4_port::BE<u16> numEntries;  // 0x00
#else
    u16 numEntries;  // 0x00
#endif
    u8 unpacked;     // 0x02
    u8 pad8;         // 0x03
#ifdef TARGET_PC
    re4_port::BE<u32> format;      // 0x04
    re4_port::Ptr32<u8> data;      // 0x08
#else
    u32 format;      // 0x04
    void* data;      // 0x08
#endif
};

struct TEXHeader {
#ifdef TARGET_PC
    re4_port::BE<u16> height;         // 0x00
    re4_port::BE<u16> width;          // 0x02
    re4_port::BE<u32> format;         // 0x04
    re4_port::Ptr32<u8> data;         // 0x08
    re4_port::BE<u32> wrapS;          // 0x0C
    re4_port::BE<u32> wrapT;          // 0x10
    re4_port::BE<u32> minFilter;      // 0x14
    re4_port::BE<u32> magFilter;      // 0x18
    re4_port::BE<f32> LODBias;        // 0x1C
#else
    u16 height;         // 0x00
    u16 width;          // 0x02
    u32 format;         // 0x04
    void* data;         // 0x08
    u32 wrapS;          // 0x0C
    u32 wrapT;          // 0x10
    u32 minFilter;      // 0x14
    u32 magFilter;      // 0x18
    f32 LODBias;        // 0x1C
#endif
    u8 edgeLODEnable;   // 0x20
    u8 minLOD;          // 0x21
    u8 maxLOD;          // 0x22
    u8 unpacked;        // 0x23
};

struct TEXDescriptor {
#ifdef TARGET_PC
    re4_port::Ptr32<TEXHeader> textureHeader;  // 0x00
    re4_port::Ptr32<CLUTHeader> CLUTHeader;    // 0x04
#else
    TEXHeader* textureHeader;  // 0x00
    CLUTHeader* CLUTHeader;    // 0x04
#endif
};

struct TEXPalette {
#ifdef TARGET_PC
    re4_port::BE<u32> version;               // 0x00
    re4_port::BE<u32> numDescriptors;        // 0x04
    re4_port::Ptr32<TEXDescriptor> descriptorArray;  // 0x08
#else
    u32 version;               // 0x00
    u32 numDescriptors;              // 0x04
    TEXDescriptor* descriptorArray;  // 0x08
#endif
};

extern "C" TEXDescriptor* TEXGet(TEXPalette* pal, u32 id);

#endif
