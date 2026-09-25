#ifndef _DOLPHIN_OSMODULE_H_
#define _DOLPHIN_OSMODULE_H_

#include <dolphin/types.h>

// Port step 1 (docs/port-boot.md): the REL header/section/import/relocation records below are read
// straight off a big-endian REL file on this little-endian host, with no swap pass of their own (REL
// loading itself is Phase 4 -- see CLAUDE.md's "REL fixups" row -- these are just the field types a
// future loader will read). include/port/be.h's BE<T> already carries this exact "physically stored
// big-endian, swap on every access" invariant for every other on-disc format's plain integer fields;
// reusing it here keeps struct layout GameCube-identical (BE<T> is sizeof(T)) while making an
// eventual raw `header->bssSize` read correct without a manual byte-swap at every call site. Hoisted
// above `extern "C" {`, not inside it: `port/be.h` brings in `namespace re4_port`, which a strict
// reading of the standard does not allow opening inside a linkage-specification block; only
// referencing the already-declared `re4_port::be_u32`/`be_u16` typedefs below, inside `extern "C"`,
// is unambiguously fine. GameCube-matching builds (no TARGET_PC) never take this branch, so their
// bytes are unaffected.
#if defined(TARGET_PC) && defined(__cplusplus)
#include "port/be.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(TARGET_PC) && defined(__cplusplus)
typedef re4_port::be_u32 OSModU32;
typedef re4_port::be_u16 OSModU16;
#else
typedef u32 OSModU32;
typedef u16 OSModU16;
#endif

#define OS_MODULE_VERSION 3
typedef struct OSModuleHeader OSModuleHeader;

typedef u32 OSModuleID;
typedef struct OSModuleQueue OSModuleQueue;
typedef struct OSModuleLink OSModuleLink;
typedef struct OSModuleInfo OSModuleInfo;
typedef struct OSSectionInfo OSSectionInfo;
typedef struct OSImportInfo OSImportInfo;
typedef struct OSRel OSRel;

struct OSModuleQueue {
    OSModuleInfo* head;
    OSModuleInfo* tail;
};

struct OSModuleLink {
    OSModuleInfo* next;
    OSModuleInfo* prev;
};

struct OSModuleInfo {
    OSModuleID id;         // unique identifier for the module
    OSModuleLink link;     // doubly linked list of modules
    OSModU32 numSections;       // # of sections
    OSModU32 sectionInfoOffset; // offset to section info table
    OSModU32 nameOffset;        // offset to module name
    OSModU32 nameSize;          // size of module name
    OSModU32 version;           // version number
};

struct OSModuleHeader {
    // CAUTION: info must be the 1st member
    OSModuleInfo info;

    // OS_MODULE_VERSION == 1
    OSModU32 bssSize; // total size of bss sections in bytes
    OSModU32 relOffset;
    OSModU32 impOffset;
    OSModU32 impSize;          // size in bytes
    u8 prologSection;     // section # for prolog function
    u8 epilogSection;     // section # for epilog function
    u8 unresolvedSection; // section # for unresolved function
    u8 bssSection;        // section # for bss section (set at run-time)
    OSModU32 prolog;           // prolog function offset
    OSModU32 epilog;           // epilog function offset
    OSModU32 unresolved;       // unresolved function offset

    // OS_MODULE_VERSION == 2
#if (2 <= OS_MODULE_VERSION)
    OSModU32 align;    // module alignment constraint
    OSModU32 bssAlign; // bss alignment constraint
#endif

    // OS_MODULE_VERSION == 3
#if (3 <= OS_MODULE_VERSION)
    OSModU32 fixSize;
#endif
};

// (u32) first: under TARGET_PC, sectionInfoOffset is OSModU32 (BE<u32>, port/be.h) -- a class type
// with only an `operator T()` conversion, not directly castable to a pointer type with one C-style
// cast (needs the explicit intermediate u32 step); a no-op on the GameCube-matching build, where
// sectionInfoOffset is already a plain u32.
#define OSGetSectionInfo(module) ((OSSectionInfo*)(u32)(((OSModuleInfo*)(module))->sectionInfoOffset))

struct OSSectionInfo {
    OSModU32 offset;
    OSModU32 size;
};

// OSSectionInfo.offset bit
#define OS_SECTIONINFO_EXEC 0x1
#define OS_SECTIONINFO_OFFSET(offset) ((offset) & ~0x1)

struct OSImportInfo {
    OSModuleID id; // external module id
    OSModU32 offset;    // offset to OSRel instructions
};

struct OSRel {
    OSModU16 offset; // byte offset from the previous entry
    u8 type;
    u8 section;
    OSModU32 addend;
};

#define R_DOLPHIN_NOP 201     //  C9h current offset += OSRel.offset
#define R_DOLPHIN_SECTION 202 //  CAh current section = OSRel.section
#define R_DOLPHIN_END 203     //  CBh
#define R_DOLPHIN_MRKREF 204  //  CCh

void OSSetStringTable(void* stringTable);
BOOL OSLink(OSModuleInfo* newModule, void* bss);

#if (3 <= OS_MODULE_VERSION)
BOOL OSLinkFixed(OSModuleInfo* newModule, void* bss);
#endif

BOOL OSUnlink(OSModuleInfo* oldModule);

OSModuleInfo* OSSearchModule(void* ptr, u32* section, u32* offset);

// debugger notification
void OSNotifyLink(OSModuleInfo* module);
void OSNotifyUnlink(OSModuleInfo* module);

#ifdef __cplusplus
}
#endif

#endif
