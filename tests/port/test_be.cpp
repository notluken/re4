// Host unit test for include/port/be.h (Phase 3, docs/port-phase3.md). No arena/disc I/O
// involved -- pure byte-order round-trip checks, the same style as test_ptr32.cpp.
#include "port/be.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

using re4_port::BE;

// A struct laid out the way an on-disc format would be: fields kept at their documented offsets,
// only the type changed from a plain integer to BE<T>. File scope (not local to main()) so
// offsetof() can be used in a static_assert.
struct OnDisc {
    BE<std::uint16_t> height; // 0x00
    BE<std::uint16_t> width;  // 0x02
    BE<std::uint32_t> format; // 0x04
};
static_assert(sizeof(OnDisc) == 8, "OnDisc layout must be unchanged by BE<T>");
static_assert(offsetof(OnDisc, height) == 0x00, "");
static_assert(offsetof(OnDisc, width) == 0x02, "");
static_assert(offsetof(OnDisc, format) == 0x04, "");

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            std::exit(1);                                                                          \
        }                                                                                           \
    } while (0)

int main()
{
    // Size / alignment / trivial-copyability, re-checked here (not just the header's own
    // static_asserts) so a failure names the actual test.
    CHECK(sizeof(BE<std::uint16_t>) == sizeof(std::uint16_t));
    CHECK(sizeof(BE<std::uint32_t>) == sizeof(std::uint32_t));
    CHECK(sizeof(BE<std::uint64_t>) == sizeof(std::uint64_t));
    CHECK(sizeof(BE<float>) == sizeof(float));
    CHECK(sizeof(BE<double>) == sizeof(double));
    CHECK(alignof(BE<std::uint32_t>) == alignof(std::uint32_t));
    CHECK(std::is_trivially_copyable<BE<std::uint32_t>>::value);
    CHECK(std::is_trivially_copyable<BE<float>>::value);

    // u8/s8: identity, no swap.
    {
        BE<std::uint8_t> b = 0x42;
        unsigned char raw;
        std::memcpy(&raw, &b, 1);
        CHECK(raw == 0x42);
        CHECK(static_cast<std::uint8_t>(b) == 0x42);
    }

    // u16: physical storage is byte-swapped relative to the host value.
    {
        BE<std::uint16_t> b = 0x1234;
        unsigned char raw[2];
        std::memcpy(raw, &b, 2);
        CHECK(raw[0] == 0x12 && raw[1] == 0x34); // big-endian on-disc order
        CHECK(static_cast<std::uint16_t>(b) == 0x1234); // reads back host-native
    }

    // u32: same check, and round-trip through a raw big-endian byte buffer (simulating a real
    // on-disc read) to confirm BE<T> interprets it as the disc intended.
    {
        unsigned char disc_bytes[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        BE<std::uint32_t> b;
        std::memcpy(&b, disc_bytes, 4);
        CHECK(static_cast<std::uint32_t>(b) == 0xDEADBEEFu);

        BE<std::uint32_t> b2 = 0xDEADBEEFu;
        unsigned char raw[4];
        std::memcpy(raw, &b2, 4);
        CHECK(raw[0] == 0xDE && raw[1] == 0xAD && raw[2] == 0xBE && raw[3] == 0xEF);
    }

    // s32: sign preserved through the swap.
    {
        BE<std::int32_t> b = -1;
        CHECK(static_cast<std::int32_t>(b) == -1);
        b = -12345;
        CHECK(static_cast<std::int32_t>(b) == -12345);
    }

    // u64.
    {
        BE<std::uint64_t> b = 0x0102030405060708ull;
        unsigned char raw[8];
        std::memcpy(raw, &b, 8);
        for (int i = 0; i < 8; ++i) {
            CHECK(raw[i] == static_cast<unsigned char>(i + 1));
        }
        CHECK(static_cast<std::uint64_t>(b) == 0x0102030405060708ull);
    }

    // f32: round-trips through the swap without corrupting the value (memcpy-based, not an
    // integer reinterpretation of the bits).
    {
        BE<float> b = 3.5f;
        CHECK(static_cast<float>(b) == 3.5f);
        float raw_le = 3.5f;
        unsigned char le_bytes[4];
        std::memcpy(le_bytes, &raw_le, 4);
        unsigned char be_bytes[4];
        std::memcpy(be_bytes, &b, 4);
        CHECK(be_bytes[0] == le_bytes[3] && be_bytes[1] == le_bytes[2] &&
              be_bytes[2] == le_bytes[1] && be_bytes[3] == le_bytes[0]);
    }

    // f64.
    {
        BE<double> b = -2.25;
        CHECK(static_cast<double>(b) == -2.25);
    }

    // Compound assignment / increment / decrement operate on the host-native value.
    {
        BE<std::uint32_t> b = 10;
        b += 5;
        CHECK(static_cast<std::uint32_t>(b) == 15);
        b -= 3;
        CHECK(static_cast<std::uint32_t>(b) == 12);
        b *= 2;
        CHECK(static_cast<std::uint32_t>(b) == 24);
        b /= 4;
        CHECK(static_cast<std::uint32_t>(b) == 6);
        ++b;
        CHECK(static_cast<std::uint32_t>(b) == 7);
        --b;
        CHECK(static_cast<std::uint32_t>(b) == 6);
        std::uint32_t old = b++;
        CHECK(old == 6 && static_cast<std::uint32_t>(b) == 7);
        old = b--;
        CHECK(old == 7 && static_cast<std::uint32_t>(b) == 6);
        b &= 0x2;
        CHECK(static_cast<std::uint32_t>(b) == 2);
        b |= 0x5;
        CHECK(static_cast<std::uint32_t>(b) == 7);
        b ^= 0x3;
        CHECK(static_cast<std::uint32_t>(b) == 4);
    }

    // Comparison.
    {
        BE<std::uint32_t> a = 100, b = 100, c = 200;
        CHECK(a == b);
        CHECK(a != c);
        CHECK(a == 100u);
        CHECK(100u == a);
        CHECK(c != 100u);
    }

    // Round-trip a raw disc-shaped buffer through OnDisc (declared above at file scope).
    {
        unsigned char disc_bytes[8] = {0x00, 0x40, 0x00, 0x80, 0x00, 0x00, 0x00, 0x06};
        OnDisc d;
        std::memcpy(&d, disc_bytes, sizeof(d));
        CHECK(static_cast<std::uint16_t>(d.height) == 0x0040);
        CHECK(static_cast<std::uint16_t>(d.width) == 0x0080);
        CHECK(static_cast<std::uint32_t>(d.format) == 0x00000006u);
    }

    std::printf("test_be: OK\n");
    return 0;
}
