#ifndef TPL_H
#define TPL_H

#include "types.h"
#ifdef TARGET_PC
#include "port/ptr32.h"
#endif

// TPL texture palette layout (charPipeline/texPalette.h without the SDK includes). File offsets
// are relocated to pointers by cTexSys::CalcTplAddr. The pointer fields below (docs/port-phase2.md,
// "the inventory") are Ptr32<T> under TARGET_PC, same reasoning as cModelData (include/model.h).
struct CLUTHeader {
    u16 numEntries;  // 0x00
    u8 unpacked;     // 0x02
    u8 pad8;         // 0x03
    u32 format;      // 0x04
#ifdef TARGET_PC
    re4_port::Ptr32<u8> data;      // 0x08
#else
    void* data;      // 0x08
#endif
};

struct TEXHeader {
    u16 height;         // 0x00
    u16 width;          // 0x02
    u32 format;         // 0x04
#ifdef TARGET_PC
    re4_port::Ptr32<u8> data;         // 0x08
#else
    void* data;         // 0x08
#endif
    u32 wrapS;          // 0x0C
    u32 wrapT;          // 0x10
    u32 minFilter;      // 0x14
    u32 magFilter;      // 0x18
    f32 LODBias;        // 0x1C
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
    u32 version;               // 0x00
    u32 numDescriptors;              // 0x04
#ifdef TARGET_PC
    re4_port::Ptr32<TEXDescriptor> descriptorArray;  // 0x08
#else
    TEXDescriptor* descriptorArray;  // 0x08
#endif
};

extern "C" TEXDescriptor* TEXGet(TEXPalette* pal, u32 id);

#endif
