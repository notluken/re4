// Host test for src/port/rel.cpp (the OSLink/OSUnlink module registry, include/port/rel.h). Uses two
// fake RelModuleDesc's (no real built REL module needed) to prove the registry's fresh-vs-relink
// contract:
//  - a fresh link (header never touched by this host) resets `data` to its pristine image and zeroes
//    `bss`;
//  - stop (OSUnlink) + restart (OSLink) without re-reading the header preserves `data`/`bss`;
//  - overwriting the header (simulating a re-read from disc) makes the next link fresh again;
//  - a second link of an already-linked id (no header change) does not reset state;
//  - OSLink() for an id with no registered descriptor returns FALSE.
#include "port/rel.h"
#include "port/rel_module.h"

#include "dolphin/os/OSModule.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                    \
    do {                                                                                                \
        if (!(cond)) {                                                                                  \
            std::fprintf(stderr, "test_rel: FAILED: %s (line %d)\n", #cond, __LINE__);                  \
            ++g_failures;                                                                               \
        }                                                                                                \
    } while (0)

void FreshHeader(OSModuleHeader* h, unsigned id)
{
    std::memset(h, 0, sizeof(*h));
    h->info.id = id;
}

void DummyProlog() {}
void DummyEpilog() {}

} // namespace

int main()
{
    // Module A: has data + bss.
    static unsigned char dataA[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    static unsigned char bssA[4] = {0xAA, 0xAA, 0xAA, 0xAA}; // simulates leftover from a prior run

    re4_port::RelModuleDesc descA{};
    descA.id = 100;
    descA.name = "fake_a";
    descA.prolog = DummyProlog;
    descA.epilog = DummyEpilog;
    descA.unresolved = nullptr;
    descA.dataStart = dataA;
    descA.dataSize = sizeof(dataA);
    descA.bssStart = bssA;
    descA.bssSize = sizeof(bssA);
    re4_port::RegisterRelModule(&descA); // snapshots dataA = {1..8} as pristine

    // Module B: registered but never linked in this test -- exercises the "missing id" path staying
    // missing for ids that ARE registered but simply not the one being looked up.
    re4_port::RelModuleDesc descB{};
    descB.id = 200;
    descB.name = "fake_b";
    descB.prolog = nullptr;
    descB.epilog = nullptr;
    descB.unresolved = nullptr;
    descB.dataStart = nullptr;
    descB.dataSize = 0;
    descB.bssStart = nullptr;
    descB.bssSize = 0;
    re4_port::RegisterRelModule(&descB);

    OSModuleHeader header;

    // 1) Fresh link: data resets to pristine, bss zeroes.
    FreshHeader(&header, 100);
    dataA[0] = 0xFF; // mutate before linking, as if a previous game instance ran and dirtied it
    CHECK(OSLink(&header.info, nullptr) == 1);
    CHECK(dataA[0] == 1 && dataA[7] == 8);
    CHECK(bssA[0] == 0 && bssA[3] == 0);

    // 2) Mutate module state (as gameplay would), then stop (OSUnlink) + restart (OSLink) without
    //    re-reading the header: state must be preserved, not reset.
    dataA[0] = 42;
    bssA[0] = 7;
    CHECK(OSUnlink(&header.info) == 1);
    CHECK(OSLink(&header.info, nullptr) == 1); // restartRelData()'s DLL_Link, same header buffer
    CHECK(dataA[0] == 42 && bssA[0] == 7);

    // 3) A second link of an already-linked id, still without touching the header, must also not
    //    reset (mirrors linkRelData() being asked to link an id that is already active).
    CHECK(OSLink(&header.info, nullptr) == 1);
    CHECK(dataA[0] == 42 && bssA[0] == 7);

    // 4) Overwriting the header (simulating DvdRead() re-reading the .rel from disc into the same
    //    buffer) makes the next link fresh again, even though the id is unchanged.
    FreshHeader(&header, 100);
    CHECK(OSLink(&header.info, nullptr) == 1);
    CHECK(dataA[0] == 1 && dataA[7] == 8);
    CHECK(bssA[0] == 0);

    // 5) Missing id: OSLink() fails.
    OSModuleHeader missing;
    FreshHeader(&missing, 999);
    CHECK(OSLink(&missing.info, nullptr) == 0);
    CHECK(OSUnlink(&missing.info) == 0);

    // 6) RelEntry() reaches the registered prolog/epilog without crashing for a known id, and is a
    //    safe no-op for an unknown one.
    static bool prologRan = false, epilogRan = false;
    re4_port::RelModuleDesc descC{};
    descC.id = 300;
    descC.name = "fake_c";
    descC.prolog = +[]() { prologRan = true; };
    descC.epilog = +[]() { epilogRan = true; };
    descC.unresolved = nullptr;
    descC.dataStart = nullptr;
    descC.dataSize = 0;
    descC.bssStart = nullptr;
    descC.bssSize = 0;
    re4_port::RegisterRelModule(&descC);
    OSModuleHeader headerC;
    FreshHeader(&headerC, 300);
    re4_port::RelEntry(&headerC, 0);
    re4_port::RelEntry(&headerC, 1);
    CHECK(prologRan && epilogRan);
    re4_port::RelEntry(&missing, 0); // must not crash

    if (g_failures != 0) {
        std::fprintf(stderr, "test_rel: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("test_rel: OK\n");
    return 0;
}
