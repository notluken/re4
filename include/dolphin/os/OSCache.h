#ifndef _DOLPHIN_OSCACHE_H_
#define _DOLPHIN_OSCACHE_H_

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

void DCInvalidateRange(void* addr, u32 nBytes);
void DCFlushRange(void* addr, u32 nBytes);
void DCStoreRange(void* addr, u32 nBytes);
void DCFlushRangeNoSync(void* addr, u32 nBytes);
void DCStoreRangeNoSync(void* addr, u32 nBytes);
void DCZeroRange(void* addr, u32 nBytes);
void DCTouchRange(void* addr, u32 nBytes);
void ICInvalidateRange(void* addr, u32 nBytes);

#ifdef TARGET_PC
// Real hardware: a fixed 16 KB scratch RAM window at a hardware address, `#define`d below for the
// matching build. No such window exists on the host; Aurora's own copy of this header (kept in
// sync by hand, `../aurora/include/dolphin/os/OSCache.h`) already has this identical branch,
// backed by a real 16 KB static host buffer (`lib/dolphin/os/OSCache.cpp`'s `s_lcData`) -- see
// docs/port-boot.md section 45/46.
extern void* LCGetBase(void);
#else
#define LC_BASE_PREFIX 0xE000
#define LC_BASE (LC_BASE_PREFIX << 16)
#define LCGetBase() ((void*)LC_BASE)
#endif

void LCEnable(void);
void LCDisable(void);
void LCLoadBlocks(void* destTag, void* srcAddr, u32 numBlocks);
void LCStoreBlocks(void* destAddr, void* srcTag, u32 numBlocks);
u32 LCLoadData(void* destAddr, void* srcAddr, u32 nBytes);
u32 LCStoreData(void* destAddr, void* srcAddr, u32 nBytes);
u32 LCQueueLength(void);
void LCQueueWait(u32 len);
void LCFlushQueue(void);
void __OSCacheInit(void);

#ifdef __cplusplus
}
#endif

#endif
