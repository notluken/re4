// See include/port/rel.h.
#ifndef TARGET_PC
#error "src/port/rel.cpp is host-only (TARGET_PC)"
#endif

#include "port/rel.h"

#include <cstdio>

namespace re4_port {

void RelEntry(OSModuleHeader* header, int kind)
{
    // Logging stub only, as documented in include/port/rel.h: no real REL module is relocated or
    // executed on this host yet, so there is nothing to call into -- the real registry (Phase 4+)
    // replaces this body without changing the DLL_PROLOG/DLL_EPILOG call sites that reach it.
    std::fprintf(stderr, "re4_port: RelEntry: %s called for module %p (stub, not calling into it)\n",
                 kind == 0 ? "prolog" : "epilog", static_cast<void*>(header));
}

} // namespace re4_port
