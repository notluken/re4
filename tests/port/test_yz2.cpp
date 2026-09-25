// Host unit test for src/game/yz2code.cpp's TARGET_PC decode loop (docs/port-phase3.md, "room data
// formats" -- the from-scratch C++ port of yz2asm.cpp's whole-function PPC asm, added in
// c4b1a979). Links the real src/game/yz2code.cpp (compiled TARGET_PC) and feeds it real yz2 streams
// -- several `stN/rNNN.das` room archives of different sizes from different stages -- decoding each
// with tools/motion/yz2.py first (the trusted, independently-written reference decoder) via
// tools/port/yz2_extract_room.py, then decoding the same bytes through Yz2DecodeSet/Yz2DecodeExec
// and comparing byte-for-byte.
//
// Needs a real GameCube disc image (orig/G4BE08/re4_debug_disc1.iso or *.gcm) at test time --
// never committed (port rules), so this test SKIPs gracefully (exit 0, one line to stderr) when
// neither is present, rather than failing CI/a fresh checkout.
#include "yz2code.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef RE4_SOURCE_DIR
#define RE4_SOURCE_DIR "."
#endif
#ifndef RE4_BINARY_DIR
#define RE4_BINARY_DIR "."
#endif

static std::vector<unsigned char> readFile(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(1);
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> data(size);
    if (size > 0 && std::fread(data.data(), 1, size, f) != (size_t) size) {
        std::fprintf(stderr, "short read on %s\n", path.c_str());
        std::exit(1);
    }
    std::fclose(f);
    return data;
}

// Extracts one room archive's yz2 stream + reference-decoded body via tools/port/yz2_extract_room.py
// (Python: FST walk + tools/motion/yz2.py). Returns false when no disc image is available.
static bool extractRoom(const std::string& room, const std::string& outRaw, const std::string& outRef)
{
    static const char* kDiscs[] = {
        RE4_SOURCE_DIR "/orig/G4BE08/re4_debug_disc1.iso",
        RE4_SOURCE_DIR "/orig/G4BE08/re4_debug_disc2.gcm",
    };
    for (const char* disc : kDiscs) {
        if (std::FILE* f = std::fopen(disc, "rb")) {
            std::fclose(f);
            std::string cmd = "python3 \"" RE4_SOURCE_DIR "/tools/port/yz2_extract_room.py\" \"" +
                               std::string(disc) + "\" \"" + room + "\" \"" + outRaw + "\" \"" + outRef + "\"";
            if (std::system(cmd.c_str()) == 0) {
                return true;
            }
            // Room not on this disc (e.g. st3/* is disc 2 only) -- try the next image.
        }
    }
    return false;
}

static void checkOne(const std::string& room, int idx, bool& anyRan)
{
    std::string outRaw = std::string(RE4_BINARY_DIR) + "/yz2_test_" + std::to_string(idx) + ".raw";
    std::string outRef = std::string(RE4_BINARY_DIR) + "/yz2_test_" + std::to_string(idx) + ".ref";
    if (!extractRoom(room, outRaw, outRef)) {
        std::fprintf(stderr, "SKIP %s: no disc image available at test time\n", room.c_str());
        return;
    }
    anyRan = true;

    std::vector<unsigned char> raw = readFile(outRaw);
    std::vector<unsigned char> ref = readFile(outRef);

    // Yz2DecodeSet mutates the header text in place (strtoul) and wants a 32-byte-aligned work
    // buffer for its `(p + 0x20) & ~0x1F` header-to-stream-start alignment (src/game/read.cpp's own
    // buffer is heap-allocated with that alignment); match it here rather than relying on malloc's
    // default alignment.
    std::vector<char> header(raw.begin(), raw.end());
    header.push_back('\0');
    void* rawBuf = nullptr;
    if (posix_memalign(&rawBuf, 32, header.size()) != 0 || !rawBuf) {
        std::fprintf(stderr, "posix_memalign failed for %s\n", room.c_str());
        std::exit(1);
    }
    std::memcpy(rawBuf, header.data(), header.size());

    void* workBuf = nullptr;
    // Generous work-buffer budget: MODEL_SETUP allocates ~(0x500+0x100)*6 bytes for the two
    // frequency tables plus 2*0x20000 for their lookup tables -- a few hundred KB is enough headroom.
    const size_t kWorkSize = 4 * 1024 * 1024;
    if (posix_memalign(&workBuf, 32, kWorkSize) != 0 || !workBuf) {
        std::fprintf(stderr, "posix_memalign failed for %s work buffer\n", room.c_str());
        std::exit(1);
    }

    u32 unpackedSize = Yz2DecodeSet((char*) rawBuf, workBuf);
    if (unpackedSize != ref.size()) {
        std::fprintf(stderr, "FAIL %s: Yz2DecodeSet size %lu != reference %zu\n", room.c_str(), (unsigned long) unpackedSize,
                      ref.size());
        std::exit(1);
    }

    std::vector<unsigned char> out(unpackedSize);
    Yz2DecodeExec(out.data());

    if (out != ref) {
        size_t mismatch = 0;
        for (; mismatch < out.size() && out[mismatch] == ref[mismatch]; mismatch++) {
        }
        std::fprintf(stderr, "FAIL %s: decoded output differs from tools/motion/yz2.py at byte %zu of %zu\n",
                      room.c_str(), mismatch, out.size());
        std::exit(1);
    }

    std::fprintf(stderr, "OK %s: %zu bytes packed -> %lu bytes unpacked, matches tools/motion/yz2.py\n",
                  room.c_str(), raw.size(), (unsigned long) unpackedSize);

    std::free(rawBuf);
    std::free(workBuf);
}

int main()
{
    // Different stages, a >3x size spread (docs/port-phase3.md's "room data formats" table lists
    // the full st1/st2/st4 size range on disc 1; st3 is disc 2, not present in this checkout).
    static const char* kRooms[] = {
        "st1/r120.das", // smallest room archive on disc 1 (~1.5 MB unpacked ~0x5ce020)
        "st1/r117.das", // mid-sized (~3.3 MB)
        "st2/r200.das", // a different stage
        "st4/r400.das", // largest room archive on disc 1 (~5.4 MB)
    };

    bool anyRan = false;
    for (size_t i = 0; i < sizeof(kRooms) / sizeof(kRooms[0]); i++) {
        checkOne(kRooms[i], (int) i, anyRan);
    }
    if (!anyRan) {
        std::fprintf(stderr, "test_yz2: SKIPPED (no orig/G4BE08 disc image found)\n");
    }
    return 0;
}
