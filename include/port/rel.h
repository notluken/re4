// Port step 1 (docs/port-boot.md), REL header shims: what a linked REL module's prolog/epilog call
// sites (include/main_sub.h's DLL_PROLOG/DLL_EPILOG, src/game/db_menu.cpp's dev-menu REL loader) do
// on this host. Calling straight into `header->prolog`'s raw file offset, as the vendor's
// DLL_PROLOG/DLL_EPILOG macros do, would jump into whatever bytes happen to be at that offset in the
// (never-relocated) REL file buffer on this host -- undefined behavior. Every DLL_PROLOG(m)/
// DLL_EPILOG(m) callsite under TARGET_PC routes through RelEntry() instead, which looks the module up
// in the registry below by `header->info.id` and calls its real prolog/epilog if one is registered
// (a logged no-op otherwise: an unbuilt module has nothing to call into) -- and then, separately,
// evaluates to a captureless no-op in place of the real prolog/epilog function pointer -- see
// include/main_sub.h.
//
// OSLink()/OSUnlink() (src/port/rel.cpp) are the host's stand-in for src/lib/OSLink.c's real
// relocator: the game still reads each REL from disc into its own heap buffer and calls these two
// exactly as on GameCube, but no relocation happens here -- the module id in that buffer's header is
// looked up in this same registry, which holds every module that was actually built and statically
// linked into this host binary (tools/port/build_rel_module.py, RelModuleDesc from
// include/port/rel_module.h). A `RegisterRelModule()` call (module ctor order, or a test) is what
// puts a descriptor in the registry; an id with none registered makes OSLink() return FALSE, which is
// what a real GameCube build does for an id that was never in a REL on the disc -- the game HALTs
// (main_sub.cpp's DLL_Link/DLL_Unlink), a correct diagnostic for "this module was not built yet".
#ifndef TARGET_PC
#error "include/port/rel.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_REL_H
#define RE4_PORT_REL_H

#include "port/rel_module.h"

struct OSModuleHeader;

namespace re4_port {

// Adds `desc` to the registry OSLink()/OSUnlink()/RelEntry() consult, keyed by `desc->id`, and
// snapshots `desc`'s current data-section bytes as the module's pristine image (this must run before
// anything mutates that data -- static/global constructor order for a real built module; explicitly,
// right after building the descriptor, for a test's fake one). Registering the same id twice is a
// caller bug (asserts).
void RegisterRelModule(const RelModuleDesc* desc);

// `kind`: 0 = prolog, 1 = epilog. Looks `header->info.id` up in the registry and calls the matching
// descriptor's prolog/epilog; logs and does nothing for an id with no registered descriptor.
void RelEntry(OSModuleHeader* header, int kind);

} // namespace re4_port

#endif // RE4_PORT_REL_H
