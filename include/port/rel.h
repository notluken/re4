// Port step 1 (docs/port-boot.md), REL header shims: a placeholder hook for what a linked REL
// module's prolog/epilog call sites (include/main_sub.h's DLL_PROLOG/DLL_EPILOG,
// src/game/db_menu.cpp's dev-menu REL loader) do on this host, since OSLink() is still a stub
// (src/port/stubs/manual_stubs.cpp) and no REL module is actually relocated or executed yet
// (Phase 4+ territory -- CLAUDE.md's "REL fixups" row). Calling straight into `header->prolog`'s
// raw file offset, as the vendor's DLL_PROLOG/DLL_EPILOG macros do, would jump into whatever bytes
// happen to be at that offset in the (never-relocated) REL file buffer on this host -- undefined
// behavior, not a graceful stub. Every DLL_PROLOG(m)/DLL_EPILOG(m) callsite under TARGET_PC routes
// through RelEntry() instead (logs and returns) and then, separately, evaluates to a captureless
// no-op in place of the real prolog/epilog function pointer -- see include/main_sub.h.
#ifndef TARGET_PC
#error "include/port/rel.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_REL_H
#define RE4_PORT_REL_H

struct OSModuleHeader;

namespace re4_port {

// `kind` distinguishes the call site for the log message only (0 = prolog, 1 = epilog) -- there is
// no real registry yet (a future pass wires this to Phase 4's static REL table, docs/port.md).
void RelEntry(OSModuleHeader* header, int kind);

} // namespace re4_port

#endif // RE4_PORT_REL_H
