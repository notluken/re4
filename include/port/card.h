// Coordinator follow-up (docs/port-boot.md section 37): gives the host's Aurora CARD emulation a
// real, port-owned, non-orig/ directory to keep its memory-card image/GCI-folder in, instead of
// falling back to Aurora's own SDL_GetPrefPath()-derived default (which works, but is not
// explicitly ours to point at a test location -- e.g. RE4_CARD_DIR -- the way the coordinator
// asked). Must be called before src/game/card.cpp's CardInit() calls Aurora's real CARDInit()
// (Aurora's own CARDSetBasePath() asserts/fatals if called after CARDInit() has already run).
#ifndef TARGET_PC
#error "include/port/card.h is TARGET_PC-only"
#endif

namespace re4_port {

// Resolves $RE4_CARD_DIR, else "<home>/Library/Application Support/re4-port/card" (macOS
// convention, matching Aurora's own SDL_GetPrefPath()-derived default location for everything
// else this port keeps outside the repo), creates it if missing, and points Aurora's CARD
// emulation (CARDSetBasePath(), both slots) at it. Never touches orig/ (no path under this repo's
// own tree is ever considered).
void InitCardDir();

} // namespace re4_port
