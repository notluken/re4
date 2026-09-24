// Phase 3 (docs/port-phase3.md), option B: big-endian accessor types.
//
// On-disc/relocated GameCube data is big-endian; this repo's arm64 host is little-endian. Rather
// than byte-swap every buffer once at load time (which would need cDvd -- format-agnostic by
// design -- to know every buffer's format, and to guarantee a swap happens exactly once even
// though data round-trips through ARAM and gets relocated/unrelocated in place), the buffer stays
// exactly as it is on disc, and on-disc struct fields are declared with a type that swaps on every
// read and write instead. This also keeps save data GameCube-compatible (no format change).
//
// BE<T> wraps a plain T, physically stored byte-swapped (i.e. in the same order the bytes have on
// disc), and swaps back on every access. sizeof(BE<T>) == sizeof(T) and alignof(BE<T>) ==
// alignof(T) always -- it has to be a drop-in replacement for a plain on-disc field, including
// inside a struct that gets bulk-copied/memclr'd/relocated the way the vendor's on-disc structs
// already are (Phase 2, include/port/ptr32.h's Ptr32<T> makes the same trivially-copyable promise
// for pointer fields).
//
// Pointer fields: Ptr32<T> (include/port/ptr32.h) is NOT wrapped in BE<T> here, and stays as-is.
// Every current Ptr32<T> use *is* an on-disc field (docs/port-phase3.md's survey), but its 4-byte
// storage is never read as a raw integer straight off disc the way e.g. TEXPalette::numDescriptors
// is: a pointer field starts life as a big-endian file-relative byte offset, and the vendor's own
// relocation code (cTexSys::CalcTplAddr and friends) reads that raw offset back out with
// Ptr32<T>::raw_handle()/FromRaw(), adds a base address, and writes the result back as a compressed
// arena handle (Phase 2) -- a value computed at runtime, in host byte order, never itself read off
// disk again. If Ptr32<T> swapped on every access, the raw pre-relocation offset would swap
// correctly on the *first* read, but the post-relocation handle written back by FromRaw() would
// then be silently mis-stored (swapped when it should not be -- it is already host-native, having
// been computed, not loaded). Making the *storage* BE-aware conflates two different lifetimes of
// the same 4 bytes; a plain, unconditional in-place byte-swap pass run once over the raw fields
// immediately after the DVD read -- before any relocation code touches them -- has no such
// conflation problem, because it runs exactly once, before relocation, and never again. So: BE<T>
// is for plain on-disc integer/float fields (sizes, counts, dimensions, format enums, ...);
// pointer/offset fields keep using Ptr32<T> exactly as Phase 2 left it, and any struct that mixes
// both kinds (TEXPalette: BE<u32> numDescriptors next to a Ptr32<TEXDescriptor> descriptorArray)
// gets one swap pass over its BE<T> fields and Ptr32<T>'s existing raw_handle()-based relocation
// code for the rest, run in that order.
#ifndef TARGET_PC
#error "include/port/be.h is host-only (TARGET_PC); it has no meaning for the original target"
#endif

#ifndef RE4_PORT_BE_H
#define RE4_PORT_BE_H

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "types.h"

namespace re4_port {

namespace detail {

inline std::uint8_t bswap(std::uint8_t v) { return v; }
inline std::uint16_t bswap(std::uint16_t v)
{
    return static_cast<std::uint16_t>((v << 8) | (v >> 8));
}
inline std::uint32_t bswap(std::uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}
inline std::uint64_t bswap(std::uint64_t v)
{
    return (std::uint64_t(bswap(static_cast<std::uint32_t>(v))) << 32) |
           std::uint64_t(bswap(static_cast<std::uint32_t>(v >> 32)));
}

// Reinterprets T's bytes as the same-sized unsigned integer, swaps, reinterprets back -- works for
// both integral T (s8/s16/.../u64) and floating-point T (f32/f64), via memcpy (no aliasing UB,
// no reliance on a particular signed-integer representation for the swap itself).
template <class T>
inline T swap_bytes(T v)
{
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "swap_bytes: unsupported size");
    using U = std::conditional_t<
        sizeof(T) == 1, std::uint8_t,
        std::conditional_t<sizeof(T) == 2, std::uint16_t,
                            std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
    U u;
    std::memcpy(&u, &v, sizeof(T));
    u = bswap(u);
    T out;
    std::memcpy(&out, &u, sizeof(T));
    return out;
}

} // namespace detail

// A big-endian-stored T. `storage` physically holds T's bytes in disc (big-endian) order at all
// times; every read/write through this wrapper swaps to/from the host's (little-endian) order.
// 1-byte T (u8/s8/bool) needs no swap at all -- BE<u8>/BE<s8> are still provided (so a struct
// definition can mark *every* on-disc field BE<T> uniformly, without special-casing the byte-sized
// ones) but swap_bytes<T> for a 1-byte T is already the identity via detail::bswap(uint8_t).
template <class T>
class BE {
    static_assert(std::is_trivially_copyable<T>::value, "BE<T>: T must be trivially copyable");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "BE<T>: unsupported T size");

public:
    BE() = default;
    BE(T v) : storage(detail::swap_bytes(v)) {}

    operator T() const { return detail::swap_bytes(storage); }

    BE& operator=(T v)
    {
        storage = detail::swap_bytes(v);
        return *this;
    }

    // Compound assignment / increment / decrement: implemented in terms of the host-order value
    // (round-trip through T) rather than touching `storage` directly, so they stay correct
    // regardless of T's swap (no special-casing needed per operator).
    BE& operator+=(T rhs) { return *this = static_cast<T>(static_cast<T>(*this) + rhs); }
    BE& operator-=(T rhs) { return *this = static_cast<T>(static_cast<T>(*this) - rhs); }
    BE& operator*=(T rhs) { return *this = static_cast<T>(static_cast<T>(*this) * rhs); }
    BE& operator/=(T rhs) { return *this = static_cast<T>(static_cast<T>(*this) / rhs); }
    BE& operator&=(T rhs) { return *this = static_cast<T>(static_cast<T>(*this) & rhs); }
    BE& operator|=(T rhs) { return *this = static_cast<T>(static_cast<T>(*this) | rhs); }
    BE& operator^=(T rhs) { return *this = static_cast<T>(static_cast<T>(*this) ^ rhs); }

    BE& operator++() { return *this = static_cast<T>(static_cast<T>(*this) + 1); }
    BE& operator--() { return *this = static_cast<T>(static_cast<T>(*this) - 1); }
    T operator++(int)
    {
        T old = static_cast<T>(*this);
        *this = static_cast<T>(old + 1);
        return old;
    }
    T operator--(int)
    {
        T old = static_cast<T>(*this);
        *this = static_cast<T>(old - 1);
        return old;
    }

    // No explicit operator==/!= here (against T, against another BE<T>, or a free-function form):
    // the implicit `operator T()` conversion above already lets a comparison against a literal, a
    // plain T, or another BE<T> resolve through the built-in operator after that one user-defined
    // conversion -- exactly the same "==0"/"== other field" call shapes this codebase's on-disc
    // structs use. Declaring a same-signature member/free operator== in addition to the conversion
    // operator creates a real ambiguity once T and the literal's type differ after promotion (T =
    // u32 = `long` under RE4_U32_32=OFF vs. a plain `int 0`, e.g. trans.cpp's
    // `tpl->numDescriptors == 0`) -- the same class of regression include/port/ptr32.h's own
    // `operator[]` comment already documents and fixes for Ptr32<T>; here the fix is simply not to
    // shadow the conversion-based path with a second candidate at all.

private:
    T storage{};
};

using be_u8 = BE<u8>;
using be_s8 = BE<s8>;
using be_u16 = BE<u16>;
using be_s16 = BE<s16>;
using be_u32 = BE<u32>;
using be_s32 = BE<s32>;
using be_u64 = BE<u64>;
using be_s64 = BE<s64>;
using be_f32 = BE<f32>;
using be_f64 = BE<f64>;

// be_u32/be_s32 are only exactly 4 bytes when RE4_U32_32 is ON (u32/s32 are the host's 8-byte
// `long` otherwise, include/types.h) -- the same pre-existing, already-documented caveat
// tools/port/gen_static_asserts.py's own comment names for every other on-disc struct field of
// this type; not a new gap BE<T> introduces. u8/s8/u16/s16/u64/s64/f32/f64 are fixed-width
// regardless of that switch, so be_u8/be_u16/be_u64/be_f32/... are always the right size.

static_assert(sizeof(BE<u8>) == sizeof(u8), "BE<u8> size");
static_assert(sizeof(BE<u16>) == sizeof(u16), "BE<u16> size");
static_assert(sizeof(BE<f32>) == sizeof(f32), "BE<f32> size");
static_assert(sizeof(BE<f64>) == sizeof(f64), "BE<f64> size");
static_assert(alignof(BE<u16>) == alignof(u16), "BE<u16> alignment");
static_assert(alignof(BE<u32>) == alignof(u32), "BE<u32> alignment");
static_assert(std::is_trivially_copyable<BE<u32>>::value, "BE<u32> must be trivially copyable");
static_assert(std::is_trivially_copyable<BE<f32>>::value, "BE<f32> must be trivially copyable");

} // namespace re4_port

#endif // RE4_PORT_BE_H
