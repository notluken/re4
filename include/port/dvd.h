// Opens the real disc image for Aurora's nod-based DVD backend (docs/port-boot.md, "Boot
// progress" -- the user decision this implements: use Aurora's `aurora_dvd_open`/`nod`, not a
// host shim over the extracted-files tree, keeping the latter as an env-var-only fallback that is
// never acted on by this file -- `nod` genuinely needs a real disc image, not a files/ directory;
// see docs/port-boot.md for why reconciling the two was left as a fallback rather than solved).
#ifndef TARGET_PC
#error "include/port/dvd.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_DVD_H
#define RE4_PORT_DVD_H

namespace re4_port {

// Resolves argv[2] (if given), else the RE4_DISC environment variable, else
// "orig/G4BE08/re4_debug_disc1.iso", and calls aurora_dvd_open() on it (must happen before any
// DVDOpen/DVDRead call -- src/game/dvd.cpp's cDvd::Init()/DvdRead(), reached from
// systemStartInit()). Aborts (via aurora_dvd_open's own fatal logging) if the image can't be
// opened -- there is no sensible fallback once a real DVD read is attempted. Never copies the
// image; opens it in place.
void InitDvd(int argc, char** argv);

// A small, ordinary linked-in-global buffer standing in for cDvd::Init()'s
// DVDGetFSTLocation() lookup (src/game/dvd.cpp). Aurora's real DVDGetFSTLocation() returns a
// pointer into its own private std::vector<FSTEntry> -- ordinary host heap memory with no
// relationship to the GC address window at all -- so it must never be fed to GC32()/GCPTR()
// (include/port/arena.h's "no game-visible pointer to host malloc memory" rule; this call site
// used to crash there with exactly that shape of bug, a 0x9xxxxxxxx-range host heap address
// reaching GC32()'s bounds check). GC32()-safe by construction: an ordinary linked-in global is
// always inside the window since docs/port-boot.md section 28's g_base-anchoring fix. Confirmed
// the resulting FstSize value is never read again anywhere in src/game or any REL module (grep),
// so this placeholder's exact address/size is inconsequential to gameplay -- it exists only so
// the log line and computation have a real, GC-mapped address instead of a foreign host one.
void* GetFstPlaceholder();

} // namespace re4_port

#endif // RE4_PORT_DVD_H
