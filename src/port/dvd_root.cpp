// See include/port/dvd_root.h.
#ifndef TARGET_PC
#error "src/port/dvd_root.cpp is host-only (TARGET_PC)"
#endif

#include "port/dvd_root.h"

#include <cstdlib>
#include <string>

namespace re4_port {

namespace {
std::string g_dvdRoot;
}

void InitDvdRoot(int argc, char** argv)
{
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        g_dvdRoot = argv[1];
        return;
    }
    if (const char* env = std::getenv("RE4_DVD_ROOT"); env != nullptr && env[0] != '\0') {
        g_dvdRoot = env;
        return;
    }
    g_dvdRoot = "orig/G4BE08/files";
}

const char* GetDvdRoot()
{
    return g_dvdRoot.c_str();
}

} // namespace re4_port
