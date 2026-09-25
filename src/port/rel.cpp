// See include/port/rel.h. Host stand-in for src/lib/OSLink.c: no REL relocation happens here, this
// just "activates" whichever statically-linked module (RelModuleDesc, registered by
// RegisterRelModule()) matches the id in the header the game read off disc.
#ifndef TARGET_PC
#error "src/port/rel.cpp is host-only (TARGET_PC)"
#endif

#include "port/rel.h"

#include "dolphin/os/OSModule.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace re4_port {

namespace {

// Sentinel stashed in a linked header's `info.link.next` to mean "this exact header buffer was
// already linked by this host" (OSLink.c's real relocator uses the same field for the real module
// queue; nothing on this host walks that queue, so it is free to reuse as a marker). A REL buffer
// freshly read off disc has never had this host write to it, so `link.next` there is whatever the
// file's own bytes are (zero, in every REL this tree has looked at) -- never this address. Address
// of a static object, not a magic integer: guaranteed unique per process, never equal to a real
// heap/stack address the game could plausibly leave lying in that field.
char g_hostLinkedMarkerStorage;
OSModuleInfo* const kHostLinkedMarker = reinterpret_cast<OSModuleInfo*>(&g_hostLinkedMarkerStorage);

struct Entry {
    const RelModuleDesc* desc;
    std::vector<unsigned char> pristineData; // desc->dataStart's contents at registration time
    bool linked = false; // for logging/diagnostics only; OSUnlink() never re-arms `fresh` detection
};

std::vector<Entry>& Registry()
{
    static std::vector<Entry> registry;
    return registry;
}

Entry* Find(unsigned id)
{
    for (Entry& e : Registry()) {
        if (e.desc->id == id) {
            return &e;
        }
    }
    return nullptr;
}

bool IsFresh(const OSModuleInfo* info)
{
    return info->link.next != kHostLinkedMarker;
}

} // namespace

void RegisterRelModule(const RelModuleDesc* desc)
{
    assert(Find(desc->id) == nullptr && "RegisterRelModule: id already registered");
    Entry e;
    e.desc = desc;
    if (desc->dataStart != nullptr && desc->dataSize != 0) {
        const unsigned char* p = static_cast<const unsigned char*>(desc->dataStart);
        e.pristineData.assign(p, p + desc->dataSize);
    }
    Registry().push_back(std::move(e));
}

void RelEntry(OSModuleHeader* header, int kind)
{
    Entry* e = Find(header->info.id);
    if (e == nullptr) {
        std::fprintf(stderr,
                     "re4_port: RelEntry: %s for unregistered module id %u (nothing to call)\n",
                     kind == 0 ? "prolog" : "epilog", static_cast<unsigned>(header->info.id));
        return;
    }
    void (*fn)() = kind == 0 ? e->desc->prolog : e->desc->epilog;
    if (fn != nullptr) {
        fn();
    }
}

} // namespace re4_port

extern "C" {

// See include/port/rel.h and the Entry/Registry() machinery above. `newModule`/`oldModule` are
// `&pModule->info` (OSModuleHeader::info is the struct's first member, main_sub.cpp's DLL_Link/
// DLL_Unlink), so `reinterpret_cast<OSModuleHeader*>` back to the enclosing header here is exactly
// what src/lib/OSLink.c itself does (`moduleHeader = (OSModuleHeader*)newModule`).
BOOL OSLink(OSModuleInfo* newModule, void* bss)
{
    auto* header = reinterpret_cast<OSModuleHeader*>(newModule);
    re4_port::Entry* e = re4_port::Find(newModule->id);
    if (e == nullptr) {
        std::fprintf(stderr, "re4_port: OSLink: no built module for id %u\n",
                     static_cast<unsigned>(newModule->id));
        return 0;
    }
    if (re4_port::IsFresh(newModule)) {
        // OSLink.c:314 semantics: a freshly-read module's bss starts zeroed. Our own module's real
        // instance data is the descriptor's compiled-in section, not the caller's `bss` buffer (that
        // buffer is never truly relocated to -- Phase 4+, "REL fixups"), so it is the descriptor's
        // own bss that gets zeroed and its own data section that gets reset to the pristine image
        // captured at RegisterRelModule() time.
        if (!e->pristineData.empty()) {
            std::memcpy(e->desc->dataStart, e->pristineData.data(), e->pristineData.size());
        }
        if (e->desc->bssStart != nullptr && e->desc->bssSize != 0) {
            std::memset(e->desc->bssStart, 0, e->desc->bssSize);
        }
        std::fprintf(stderr, "re4_port: OSLink: module '%s' (id %u) fresh link\n", e->desc->name,
                     e->desc->id);
    } else {
        std::fprintf(stderr, "re4_port: OSLink: module '%s' (id %u) relink (state preserved)\n",
                     e->desc->name, e->desc->id);
    }
    newModule->link.next = re4_port::kHostLinkedMarker;
    newModule->link.prev = nullptr;
    e->linked = true;
    (void) header;
    (void) bss;
    return 1;
}

BOOL OSLinkFixed(OSModuleInfo* newModule, void* bss)
{
    // No statically-linked module registers as "fixed"-only yet; same host behavior as OSLink().
    return OSLink(newModule, bss);
}

BOOL OSUnlink(OSModuleInfo* oldModule)
{
    re4_port::Entry* e = re4_port::Find(oldModule->id);
    if (e == nullptr || !e->linked) {
        std::fprintf(stderr, "re4_port: OSUnlink: module id %u not linked\n",
                     static_cast<unsigned>(oldModule->id));
        return 0;
    }
    // Deliberately does not touch `oldModule->link.next` (the fresh-vs-relink marker): the header
    // buffer was not re-read from disc, so a following OSLink() of the same buffer (restartRelData(),
    // roomdata.cpp) must still see it as "already linked" and preserve state, matching the real
    // stop/restart contract.
    e->linked = false;
    std::fprintf(stderr, "re4_port: OSUnlink: module '%s' (id %u) unlinked\n", e->desc->name,
                 e->desc->id);
    return 1;
}

} // extern "C"
