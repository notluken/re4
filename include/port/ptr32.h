// Phase 2 (docs/port-phase2.md), strategy D: compressed 32-bit handles relative to a host base.
//
// The GameCube's on-disc/relocated formats (models, TPLs, save data, ...) store 32-bit pointer
// fields that get relocated in place at load time into real 0x80xxxxxx-range addresses. On this
// 64-bit host those fields can't hold a real pointer, and widening them breaks every struct's
// layout and every piece of code that treats "sign bit set" as "already relocated" (VALID_PTR,
// SysMem's 0x80000000-0x817F4000 map, ...).
//
// Instead: a fixed 4 GiB host arena is reserved once at startup (src/port/arena.cpp) at a host
// address A, and every on-disc pointer field is compressed to a 32-bit "handle" h = host_addr - g_base,
// where g_base = A - 0x80000000. As long as everything the game ever points at (the exe image, the
// main stack, the arena) lives in the 4 GiB window [g_base, g_base + 0x100000000), h looks exactly
// like the GameCube address the field would have held (same sign bit, same SysMem range for arena
// pointers), and GCPTR(h) recovers a real, dereferenceable host pointer. This header only makes
// sense once that window is guaranteed, so it is TARGET_PC-only.
#ifndef TARGET_PC
#error "include/port/ptr32.h is host-only (TARGET_PC); it has no meaning for the original target"
#endif

#ifndef RE4_PORT_PTR32_H
#define RE4_PORT_PTR32_H

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "types.h"

namespace re4_port {

// Set once by InitArena() (src/port/arena.cpp) before any GC32/GCPTR/Ptr32 use. 0 is not a valid
// base (it would make every host pointer >= 0 look like a valid handle); InitArena() never leaves
// it at 0.
extern std::uintptr_t g_base;

// The compressed-handle window is the full 32-bit range starting at g_base: g_base itself is chosen
// (src/port/arena.cpp) so every address the game can point at falls in
// [g_base, g_base + kWindowSize).
inline constexpr std::uint64_t kWindowSize = std::uint64_t(1) << 32;

// Host pointer -> GameCube-looking 32-bit handle. nullptr -> 0 (so GCPTR(GC32(p)) == p holds for
// p == nullptr too, without g_base needing to be 0). Debug builds assert the pointer is inside the
// reserved window; a pointer outside it would silently alias a different handle, corrupting the
// on-disc format it is being written into.
template <class T>
inline std::uint32_t GC32(T* p)
{
    if (p == nullptr) {
        return 0;
    }
    assert(g_base != 0 && "GC32: g_base not set (InitArena() not called yet)");
    std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(p);
    assert(addr >= g_base && (addr - g_base) < kWindowSize &&
           "GC32: pointer outside the compressed-handle window (see include/port/ptr32.h)");
    return static_cast<std::uint32_t>(addr - g_base);
}

// GameCube-looking 32-bit handle -> host pointer. 0 -> nullptr.
template <class T = void>
inline T* GCPTR(std::uint32_t h)
{
    if (h == 0) {
        return nullptr;
    }
    assert(g_base != 0 && "GCPTR: g_base not set (InitArena() not called yet)");
    return reinterpret_cast<T*>(g_base + h);
}

// A 4-byte on-disc pointer field. Implicitly convertible to/from T* (so existing code that reads or
// assigns the field keeps working unchanged); explicit only to the raw 32-bit handle, since that
// value is meaningless without g_base. Trivially copyable (a plain u32 under the hood) so it can sit
// in a struct that gets bulk-copied/relocated/memset like the vendor's on-disc structs already are.
//
// Storage is ALWAYS big-endian (Phase 3, docs/port-phase3.md), whether the field currently holds a
// raw pre-relocation file offset or an already-relocated compressed handle -- the same invariant
// include/port/be.h's BE<T> keeps for plain integer/float fields, applied here too. This was NOT
// the original design (a first pass stored the post-relocation handle host-native/unswapped, only
// swapping the raw pre-relocation read on demand via a since-removed raw_handle_be()) and that
// design was a real, latent bug, found by inspection rather than by a crash:
//   - The "already relocated?" sign-bit guard used throughout this codebase (`(s32) field >= 0` /
//     `(s32) field.raw_handle() >= 0`, cam_ctrl.cpp/game.cpp/mes.cpp/model.cpp/...) is only correct
//     if reading the field's raw bytes as a plain native s32 gives the same sign both before and
//     after relocation. With mixed storage (BE pre-relocation, host-native post-relocation) it does
//     not: a small on-disc offset like 0x000000C0 is stored in memory as bytes 00 00 00 C0 (its
//     semantic MSB -- always zero for a small offset -- at the *lowest* address, BE convention);
//     reading those same 4 bytes back as a native little-endian int reinterprets the *last* byte
//     (0xC0, actually the offset's LSB) as the sign byte, giving 0xC0000000 -- negative, i.e. a false
//     "already relocated". Any raw offset whose low byte is >= 0x80 broke the guard; this went
//     unnoticed in-session only because the specific offsets exercised so far happened to have a
//     low byte < 0x80.
//   - The inverse-relocation direction (`RelocPtrToOffset`/`SlidePtr`, src/game/model.cpp; the
//     equivalent in src/game/game.cpp for save data) computed a plain host-native numeric offset and
//     wrote it back through FromRaw() with no swap -- so a relocate -> unrelocate -> relocate round
//     trip (or a save write) would silently flip these fields' byte order relative to a real
//     GameCube's, breaking save compatibility and corrupting a second relocation pass.
// Storing every Ptr32<T> field big-endian at all times, and swapping on every read/write through
// this class (never anywhere else -- see below), fixes both: the sign-bit guard now reads the same
// swapped value whether or not relocation has happened yet (a real GameCube's CPU is *always*
// big-endian, so it never had two different interpretations to begin with -- this restores that),
// and FromRaw()/raw_handle() are exact inverses of each other regardless of how many times they are
// composed, so relocate/unrelocate round-trips and save writes stay byte-identical to a real
// GameCube's.
//
// This means every existing call site that already used raw_handle() for pointer-relocation
// arithmetic (cam_ctrl.cpp, card.cpp, game.cpp, model.cpp, room_jmp.cpp, texture.cpp, trans.cpp)
// needed NO call-site changes -- raw_handle() now does the right thing uniformly, pre- or
// post-relocation, by construction. (src/game/mes.cpp's since-reverted raw_handle_be() workaround
// is exactly what this class-wide fix replaces; it was correct for the one call site it touched but
// left every other user of raw_handle() still broken, per the "systematic, not ad hoc" project
// rule.)
template <class T>
class Ptr32 {
public:
    Ptr32() = default;
    // No separate Ptr32(std::nullptr_t) overload: the vendor's own code assigns a field its "empty"
    // value with a plain `field = 0;` as often as `field = nullptr;` (both are null pointer
    // constants), and a literal 0 converts to *both* nullptr_t and T* equally well, which is
    // ambiguous between two constructors -- one is enough, since a literal 0 or nullptr both convert
    // to T* directly (a null pointer constant), and GC32(nullptr) already returns 0 below.
    Ptr32(T* p) { store(GC32(p)); }

    // Some on-disc formats reuse a pointer field to also hold a small plain integer (typically a
    // file-relative byte offset, before the field is relocated into a real pointer -- e.g.
    // cModelData::pClr before calcModelAddr runs). FromRaw/raw_handle give direct access to that
    // value (as an ordinary host-native u32 -- the swap to/from the field's actual big-endian
    // storage happens inside store()/load(), not at these call sites) for exactly that case;
    // nothing else should need them (ordinary pointer use goes through the constructor/conversion
    // operators below, which route through GC32/GCPTR the same way).
    static Ptr32<T> FromRaw(std::uint32_t raw)
    {
        Ptr32<T> p;
        p.store(raw);
        return p;
    }
    std::uint32_t raw_handle() const { return load(); }

    operator T*() const { return GCPTR<T>(load()); }
    // A C-style/explicit cast only looks for a conversion function whose return type matches the
    // target exactly (or is reached by a *further* user-defined conversion, which overload
    // resolution does not chain for explicit operators) -- so these have to be spelled `u32`/`s32`,
    // the game's own typedefs, rather than fixed-width std:: types: existing code casts these fields
    // with `(u32) field`/`(s32) field`/`(int) field` (calcModelAddr and friends, `(s32) descriptorArray
    // < 0`), and u32/s32 are 4 or 8 bytes depending on RE4_U32_32 (include/types.h). Widening the
    // stored 4-byte handle into an 8-byte u32/s32 when RE4_U32_32 is off loses nothing. `s32` and
    // `int` are the same type when RE4_U32_32 is on (both 4-byte int), so only one of the two
    // operators is declared then -- a duplicate declaration is otherwise an error.
    explicit operator u32() const { return static_cast<u32>(load()); }
#if defined(RE4_U32_32)
    explicit operator int() const { return static_cast<int>(load()); }
#else
    explicit operator int() const { return static_cast<int>(load()); }
    explicit operator s32() const { return static_cast<s32>(load()); }
#endif

    T* operator->() const { return GCPTR<T>(load()); }
    // Templated on the index type (rather than a fixed std::size_t) so overload resolution always
    // has an exact match on the index argument, same as it would with GCPTR<T>(m_handle)[i] written
    // out longhand: with a fixed std::size_t parameter and RE4_U32_32 off (where s32 is `long`,
    // distinct from `int`/std::size_t), a `long`-typed index (as game code passing an `s32` commonly
    // is, e.g. cam_ctrl.cpp's `pArea->points[(i + 1) % pArea->num]`) needed a standard conversion to
    // std::size_t for this member operator[], but *also* qualified the compiler-synthesized built-in
    // `operator[](T*, long)` (via the T* conversion operator above) -- two equally-ranked candidates,
    // an ambiguous call, a real regression the RE4_U32_32=OFF build did not have before Ptr32<T>
    // existed. An exact-match template parameter here makes the member operator strictly better on
    // the index argument (identity vs. the built-in's implicit-object-conversion tax), so it always
    // wins outright.
    template <class Index, class = std::enable_if_t<std::is_integral<Index>::value>>
    T& operator[](Index i) const { return GCPTR<T>(load())[static_cast<std::ptrdiff_t>(i)]; }

    bool operator==(std::nullptr_t) const { return m_handle == 0; }
    bool operator!=(std::nullptr_t) const { return m_handle != 0; }

private:
    // Always big-endian (disk order), regardless of whether the field currently holds a raw
    // pre-relocation offset or an already-relocated handle -- see the class comment above. load()/
    // store() are the ONLY place that ever swaps; every accessor above goes through one of them, and
    // nothing outside this class should ever touch m_handle directly (a raw `(u32*)&field` read/
    // write anywhere in this tree bypasses the swap and must not exist -- docs/port-phase3.md's
    // survey looked for this pattern and found none on the Ptr32<T>-converted fields).
    static std::uint32_t swap(std::uint32_t v)
    {
        return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
               ((v & 0xFF000000u) >> 24);
    }
    std::uint32_t load() const { return swap(m_handle); }
    void store(std::uint32_t v) { m_handle = swap(v); }

    std::uint32_t m_handle;
};

// Checked against Ptr32<int> rather than Ptr32<void>: T& operator[] is only ever instantiated when
// actually called, but its declaration still needs a valid T&, and T = void has no such thing.
static_assert(sizeof(Ptr32<int>) == 4, "Ptr32<T> must be exactly 4 bytes (an on-disc pointer field)");
static_assert(std::is_trivially_copyable<Ptr32<int>>::value, "Ptr32<T> must be trivially copyable");

// GC32(someField) where someField is already a Ptr32<T> (e.g. VALID_PTR(pBlock->m_pList),
// include/main_mem.h): template argument deduction can't implicitly convert a class argument to
// match `GC32(T* p)` above (deduction requires the argument to already look like a pointer), so this
// overload is needed even though Ptr32<T> converts to T* implicitly everywhere else. Its raw_handle()
// already *is* the GC32 value (that's what the constructor computed), so no recomputation needed.
template <class T>
inline std::uint32_t GC32(const Ptr32<T>& p)
{
    return p.raw_handle();
}

} // namespace re4_port

#endif // RE4_PORT_PTR32_H
