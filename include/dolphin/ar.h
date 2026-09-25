#ifndef _DOLPHIN_AR_H_
#define _DOLPHIN_AR_H_

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// TARGET_PC: Aurora's own ARQPostRequest (../aurora/lib/dolphin/AR.cpp, ../aurora/include/dolphin/
// arq.h) declares/invokes its callback with a real, full-width host pointer (`uintptr_t`), not the
// GameCube's 32-bit one -- confirmed live with lldb: `AR.cpp` handed a callback
// `request=0x100d35378` (a real 64-bit host pointer to the ARQRequest), which a `u32`-typed
// callback parameter truncates on receipt (the low 32 bits only), long before any of this port's
// own GC32()/GCPTR() translation runs -- the actual root cause of a SIGSEGV inside `trans2aram_cb`
// while loading `rel/Sscrn.rel` (docs/port-boot.md). Widening the typedef/prototype here (not
// touched by the cast rewriter -- it only rewrites cast expressions, not declarations) is what
// makes the call's actual machine ABI match what Aurora really compiled; see src/game/dvd.cpp's
// own `trans2aram_cb`/`aram_cb` for the matching TARGET_PC parameter-type change. Every other
// field/parameter here (`owner`, `type`, `priority`, `length`, and `ARQRequest`'s own storage)
// stays plain `u32`: those are never dereferenced as a host pointer by Aurora itself, only ever
// round-tripped through this port's own GC32()/GCPTR() by the game code that reads them back
// (already correct, confirmed against the cast rewriter's generated output). Non-TARGET_PC
// (the byte-matching build) is untouched.
#ifdef TARGET_PC
typedef void (*ARQCallback)(unsigned long pointerToARQRequest);
#else
typedef void (*ARQCallback)(u32 pointerToARQRequest);
#endif

struct ARQRequest {
    /* 0x00 */ struct ARQRequest *next;
    /* 0x04 */ u32 owner;
    /* 0x08 */ u32 type;
    /* 0x0C */ u32 priority;
    /* 0x10 */ u32 source;
    /* 0x14 */ u32 dest;
    /* 0x18 */ u32 length;
    /* 0x1C */ ARQCallback callback;
};

#define ARQ_DMA_ALIGNMENT 32

#define ARAM_DIR_MRAM_TO_ARAM 0x00
#define ARAM_DIR_ARAM_TO_MRAM 0x01

#define ARStartDMARead(mmem, aram, len) \
    ARStartDMA(ARAM_DIR_ARAM_TO_MRAM, mmem, aram, len)
#define ARStartDMAWrite(mmem, aram, len) \
    ARStartDMA(ARAM_DIR_MRAM_TO_ARAM, mmem, aram, len)

typedef struct ARQRequest ARQRequest;

#define ARQ_TYPE_MRAM_TO_ARAM ARAM_DIR_MRAM_TO_ARAM
#define ARQ_TYPE_ARAM_TO_MRAM ARAM_DIR_ARAM_TO_MRAM

#define ARQ_PRIORITY_LOW  0
#define ARQ_PRIORITY_HIGH 1

// AR
ARQCallback ARRegisterDMACallback(ARQCallback callback);
u32 ARGetDMAStatus(void);
void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length);
u32 ARAlloc(u32 length);
u32 ARFree(u32* length);
BOOL ARCheckInit(void);
u32 ARInit(u32* stack_index_addr, u32 num_entries);
void ARReset(void);
void ARSetSize(void);
u32 ARGetBaseAddress(void);
u32 ARGetSize(void);
u32 ARGetInternalSize(void);
void ARClear(u32 flag);

// ARQ
void ARQInit(void);
void ARQReset(void);
// TARGET_PC: `source`/`dest` widened to match Aurora's real ARQPostRequest exactly (../aurora/
// include/dolphin/arq.h) -- whichever side of a transfer is the real-MRAM side is a literal host
// pointer to Aurora (`(u8*)(uintptr_t)source`/`dest`, ../aurora/lib/dolphin/AR.cpp), not a 32-bit
// GameCube address; see the matching call-site comment (src/game/dvd.cpp's `trans2aram`) for how a
// caller now passes that side. A plain `u32` still implicitly widens into this with no cast needed
// (every existing, not-yet-updated call site keeps compiling, unchanged in behavior). Non-
// TARGET_PC (the byte-matching build) is untouched.
#ifdef TARGET_PC
void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority, unsigned long source, unsigned long dest, u32 length, ARQCallback callback);
#else
void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length, ARQCallback callback);
#endif
void ARQRemoveRequest(ARQRequest* request);
void ARQRemoveOwnerRequest(u32 owner);
void ARQFlushQueue(void);
void ARQSetChunkSize(u32 size);
u32 ARQGetChunkSize(void);
BOOL ARQCheckInit(void);

#ifdef __cplusplus
}
#endif

#endif
