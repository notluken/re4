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
template <class T>
class Ptr32 {
public:
    Ptr32() = default;
    Ptr32(std::nullptr_t) : m_handle(0) {}
    Ptr32(T* p) : m_handle(GC32(p)) {}

    operator T*() const { return GCPTR<T>(m_handle); }
    explicit operator std::uint32_t() const { return m_handle; }
    explicit operator int() const { return static_cast<int>(m_handle); }

    T* operator->() const { return GCPTR<T>(m_handle); }
    T& operator[](std::size_t i) const { return GCPTR<T>(m_handle)[i]; }

    bool operator==(std::nullptr_t) const { return m_handle == 0; }
    bool operator!=(std::nullptr_t) const { return m_handle != 0; }

private:
    std::uint32_t m_handle;
};

// Checked against Ptr32<int> rather than Ptr32<void>: T& operator[] is only ever instantiated when
// actually called, but its declaration still needs a valid T&, and T = void has no such thing.
static_assert(sizeof(Ptr32<int>) == 4, "Ptr32<T> must be exactly 4 bytes (an on-disc pointer field)");
static_assert(std::is_trivially_copyable<Ptr32<int>>::value, "Ptr32<T> must be trivially copyable");

} // namespace re4_port

#endif // RE4_PORT_PTR32_H
