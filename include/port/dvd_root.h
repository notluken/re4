// re4_boot's DVD root path (docs/port-boot.md "Boot progress"): where to find the extracted disc
// tree (orig/G4BE08/files/) or a disc image, once DVD reads are reached. Not yet wired past
// storage -- boot currently stops before cDvd::Init() (docs/port-boot.md section 8's MEM1/arena
// blocker) -- but the configuration surface (argv[1] / RE4_DVD_ROOT env var) is independent of
// that and set up now so whichever DVD backend gets picked (Aurora's `aurora_dvd_open`, which
// wants a real disc image via `nod`, not an extracted-files tree -- a second, separate design
// question, see docs/port-boot.md) has a path to read from without another plumbing pass.
#ifndef TARGET_PC
#error "include/port/dvd_root.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_DVD_ROOT_H
#define RE4_PORT_DVD_ROOT_H

namespace re4_port {

// Resolves argv[1] (if given) or the RE4_DVD_ROOT environment variable (if set), else
// "orig/G4BE08/files" relative to the current working directory. Call once, early in main().
void InitDvdRoot(int argc, char** argv);

// The resolved path (never null after InitDvdRoot()); "" if InitDvdRoot() was never called.
const char* GetDvdRoot();

} // namespace re4_port

#endif // RE4_PORT_DVD_ROOT_H
