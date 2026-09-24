// Host unit test for include/port/ptr32.h (Phase 2 step 2, docs/port-phase2.md). No game code
// involved: a fake "arena" is just a heap buffer, exercised the same way InitArena() would set
// g_base from a real one (src/port/arena.cpp, step 3).
#include "port/ptr32.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

using re4_port::g_base;
using re4_port::GC32;
using re4_port::GCPTR;
using re4_port::Ptr32;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            std::exit(1);                                                                          \
        }                                                                                           \
    } while (0)

int main()
{
    // sizeof / trivially copyable, at namespace scope already via static_assert in the header;
    // re-checked here so a test failure names the actual test, not just a header static_assert.
    CHECK(sizeof(Ptr32<int>) == 4);
    CHECK(std::is_trivially_copyable<Ptr32<int>>::value);

    // A fake 64 KiB "arena": g_base picked so this buffer's addresses look like GameCube ones (the
    // low bytes of a typical malloc'd address are unpredictable, so g_base is derived from the
    // buffer itself rather than a fixed constant -- src/port/arena.cpp does the equivalent from the
    // real reserved region instead).
    unsigned char* arena = static_cast<unsigned char*>(std::malloc(0x10000));
    CHECK(arena != nullptr);
    g_base = reinterpret_cast<std::uintptr_t>(arena) - 0x80000000u;

    // Round trip: a real pointer into the arena survives GC32 -> GCPTR unchanged.
    int* p = reinterpret_cast<int*>(arena + 0x100);
    *p = 0x1234;
    std::uint32_t h = GC32(p);
    int* back = GCPTR<int>(h);
    CHECK(back == p);
    CHECK(*back == 0x1234);

    // NULL round trip.
    CHECK(GC32<int>(nullptr) == 0);
    CHECK(GCPTR<int>(0) == nullptr);

    // GC32(arena) looks like a GameCube address (sign bit set, "already relocated"): with g_base =
    // arena - 0x80000000, GC32(arena) == 0x80000000 exactly, so (s32) it is negative.
    std::uint32_t h_arena = GC32(arena);
    CHECK(h_arena == 0x80000000u);
    CHECK(static_cast<int>(h_arena) < 0);

    // Ptr32<T>: implicit to/from T*, ->, [].
    struct Node {
        int value;
        Ptr32<Node> next;
    };
    Node* n0 = reinterpret_cast<Node*>(arena + 0x200);
    Node* n1 = reinterpret_cast<Node*>(arena + 0x300);
    n0->value = 1;
    n0->next = n1;
    n1->value = 2;
    n1->next = nullptr;

    CHECK(sizeof(Ptr32<Node>) == 4);
    CHECK(std::is_trivially_copyable<Ptr32<Node>>::value);
    Ptr32<Node> h0 = n0;
    CHECK(static_cast<Node*>(h0) == n0);
    CHECK(h0->value == 1);
    CHECK(static_cast<Node*>(h0->next) == n1);
    CHECK(h0->next->value == 2);
    CHECK(h0->next->next == nullptr);

    Ptr32<int> arr = reinterpret_cast<int*>(arena + 0x400);
    arr[0] = 10;
    arr[1] = 20;
    CHECK(arr[0] == 10 && arr[1] == 20);

    // Phase 3 (docs/port-phase3.md): Ptr32<T> storage is always big-endian, whether the field
    // currently holds a raw pre-relocation file offset or an already-relocated handle -- the
    // coordinator's diagnosis of the mixed-storage design this replaced. Simulates a real on-disc
    // read (raw bytes written directly, big-endian, bypassing Ptr32<T> the way a DVD read would)
    // for an offset whose low byte is >= 0x80 -- exactly the value class that broke the old,
    // partially-swapped design's sign-bit guard (0x000000C0: BE bytes 00 00 00 C0, reinterpreted
    // raw as a native little-endian s32 that would misread as negative).
    struct OnDiscNode {
        Ptr32<int> field; // 0x00, big-endian on disc, like a real GameCube pointer field
    };
    OnDiscNode* disc = reinterpret_cast<OnDiscNode*>(arena + 0x500);
    unsigned char be_offset[4] = {0x00, 0x00, 0x00, 0xC0}; // 0x000000C0, big-endian
    std::memcpy(&disc->field, be_offset, 4);

    // 1. Raw read before relocation: raw_handle() must decode the true numeric offset (0xC0), not
    //    the byte-swapped 0xC0000000 a fully-unswapped design would have returned.
    CHECK(disc->field.raw_handle() == 0x000000C0u);

    // 2. The "already relocated?" sign-bit guard, unswapped storage would have gotten wrong for
    //    this exact offset (low byte >= 0x80): must read as "not yet relocated" (>= 0).
    CHECK((int) disc->field.raw_handle() >= 0);

    // 3. Relocate: offset -> real pointer (same pattern as model.cpp's RelocOffsetToPtr /
    //    mes.cpp's MessageFont::create).
    int* real = reinterpret_cast<int*>(arena + 0x600);
    *real = 0x5A5A;
    disc->field = (int*) ((unsigned char*) disc + disc->field.raw_handle());
    CHECK(static_cast<int*>(disc->field) == reinterpret_cast<int*>((unsigned char*) disc + 0xC0));

    // 4. Now the sign-bit guard must read the *other* way (an arena handle's top bit is set):
    //    "already relocated". If load()/store() ever regressed back to mixed storage, this would
    //    silently re-enter the relocation branch on a live pointer, corrupting it.
    CHECK((int) disc->field.raw_handle() < 0);

    // 5. Unrelocate: pointer -> offset (RelocPtrToOffset/game.cpp's inverse-relocation direction),
    //    using the same base GC32(disc) the relocation added.
    std::uint32_t gc_base = GC32(reinterpret_cast<void*>(disc));
    disc->field = Ptr32<int>::FromRaw(disc->field.raw_handle() - gc_base);
    CHECK(disc->field.raw_handle() == 0x000000C0u);

    // 6. Byte-identical round trip: the field's raw storage bytes must match the original on-disc
    //    big-endian bytes exactly (this is the save-compatibility half of the coordinator's bug
    //    report -- a relocate/unrelocate round trip must not silently change byte order).
    unsigned char roundtrip_bytes[4];
    std::memcpy(roundtrip_bytes, &disc->field, 4);
    CHECK(std::memcmp(roundtrip_bytes, be_offset, 4) == 0);

    // 7. Relocate a second time: must reach the exact same real pointer as step 3.
    disc->field = (int*) ((unsigned char*) disc + disc->field.raw_handle());
    CHECK(static_cast<int*>(disc->field) == reinterpret_cast<int*>((unsigned char*) disc + 0xC0));

    std::free(arena);
    std::printf("test_ptr32: OK\n");
    return 0;
}
