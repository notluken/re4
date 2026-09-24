// See include/port/dvd.h.
#ifndef TARGET_PC
#error "src/port/dvd.cpp is host-only (TARGET_PC)"
#endif

#include "port/dvd.h"

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
