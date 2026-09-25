// See include/port/card.h.
#ifndef TARGET_PC
#error "src/port/card.cpp is host-only (TARGET_PC)"
#endif

#include "port/card.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

// Aurora's own CARDSetBasePath(const char*, s32) (../aurora/lib/dolphin/card.cpp) -- not declared
// in this repo's own include/dolphin/card.h (that header mirrors the real hardware's zero-extra-
// parameter CARDInit() family and has no host-only base-path concept), and Aurora's own copy of
// the same header can't be included here either: this repo's RE4_GAME_INCLUDES is searched first
// (even for src/port/ targets) and its <dolphin/card.h> wins the `#include <dolphin/card.h>`
// lookup regardless, plus it has an unrelated pre-existing `new`-as-parameter-name error
// (`CARDRename`) that only ever surfaces if something actually parses that far into the file.
// A local extern "C" declaration (matching Aurora's real, `extern "C"`, ordinary-linkage symbol)
// sidesteps both problems, same pattern as this repo's other manual externs for Aurora-only
// globals not in any public header (e.g. src/port/mem1.cpp's MEM1Start/MEM1End).
extern "C" void CARDSetBasePath(const char* path, int chan);

namespace re4_port {

void InitCardDir()
{
    std::filesystem::path dir;
    if (const char* env = std::getenv("RE4_CARD_DIR")) {
        dir = env;
    } else if (const char* home = std::getenv("HOME")) {
        dir = std::filesystem::path(home) / "Library" / "Application Support" / "re4-port" / "card";
    } else {
        // No $HOME (unusual, but don't crash boot over it) -- fall back to Aurora's own
        // CARDInit()-time default (its own userPath-or-cwd logic, ../aurora/lib/dolphin/card.cpp),
        // by simply not calling CARDSetBasePath() at all.
        std::fprintf(stderr, "re4_port: InitCardDir() -- no $RE4_CARD_DIR and no $HOME, "
                              "leaving Aurora's own default card path in place\n");
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "re4_port: InitCardDir() -- create_directories(%s) failed: %s, "
                              "leaving Aurora's own default card path in place\n",
                      dir.string().c_str(), ec.message().c_str());
        return;
    }

    std::fprintf(stderr, "re4_port: card dir=%s\n", dir.string().c_str());
    CARDSetBasePath(dir.string().c_str(), -1); // -1: both slots (Aurora's own convention, any
                                                // value other than 0/1 -- ../aurora/lib/dolphin/
                                                // card.cpp's CARDSetBasePath()).
}

} // namespace re4_port
