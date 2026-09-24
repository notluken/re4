// See include/port/dvd.h.
#ifndef TARGET_PC
#error "src/port/dvd.cpp is host-only (TARGET_PC)"
#endif

#include "port/dvd.h"

// <cstdint> before <aurora/dvd.h>: that header wants int64_t/int32_t/uint8_t straight from its own
// #include <dolphin/types.h>, but this project's own include/ dir (RE4_GAME_INCLUDES) is searched
// before Aurora's own include dir for this target (CMakeLists.txt), so the angle-bracket
// `<dolphin/types.h>` it names resolves to *this repo's* copy instead -- which, unlike Aurora's,
// never pulls in <stdint.h> (it typedefs s8/u8/... by hand instead). Surfaced only once TARGET_PC
// became global for Aurora's own build too (docs/port-boot.md section 26); harmless to include
// <cstdint> explicitly here regardless of which dolphin/types.h wins.
#include <cstdint>

#include <aurora/dvd.h>

#include <cstdio>
#include <cstdlib>

namespace re4_port {

void InitDvd(int argc, char** argv)
{
    const char* path = "orig/G4BE08/re4_debug_disc1.iso";
    if (argc > 2 && argv[2] != nullptr && argv[2][0] != '\0') {
        path = argv[2];
    } else if (const char* env = std::getenv("RE4_DISC"); env != nullptr && env[0] != '\0') {
        path = env;
    }
    std::fprintf(stderr, "re4_boot: disc image=%s\n", path);
    if (!aurora_dvd_open(path)) {
        std::fprintf(stderr, "re4_boot: aurora_dvd_open(%s) failed\n", path);
        std::abort();
    }
}

} // namespace re4_port
