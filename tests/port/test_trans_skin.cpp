// Host unit test for src/game/trans.cpp's TARGET_PC substitutions for CalcSk1_x/CalcSk1_x2/
// setupGQR6 (docs/port-boot.md section 46) -- the vertex-skinning inner loop's whole-function
// vendor asm. Not testing re4_port itself: links straight against src/game/trans.cpp, same
// pattern as tests/port/test_math_sub.cpp.
//
// The reference vectors in trans_skin_vectors.h are NOT hand-computed here -- they come from
// running the vendor's own asm text (copied verbatim from this same trans.cpp) through
// tools/port/ppcsim's from-scratch PowerPC interpreter on randomized synthetic (non-disc) inputs
// (tools/port/ppcsim/test_calc_sk1.py, which also verifies its copy of the asm text still matches
// this file). This test recomputes nothing: it feeds the same inputs into the actual compiled
// C++ port and diffs the output against those vectors bit-for-bit.
#include "types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "trans_skin_vectors.h"

// trans.cpp's own CalcSk1_x/CalcSk1_x2/setupGQR6 (extern "C", per include/trans.h).
extern "C" {
void CalcSk1_x(void* dst, void* src, u32 n);
void CalcSk1_x2(void* dst, void* src, u32 n);
void setupGQR6(u32 v);
}

// trans.cpp's TARGET_PC port calls LCGetBase() (include/dolphin/os/OSCache.h's `extern void*
// LCGetBase(void);` branch); provide the same real-buffer implementation Aurora's
// lib/dolphin/os/OSCache.cpp uses for re4_boot, so this standalone test does not need to link
// Aurora at all.
alignas(32) static u8 s_lcData[16 * 1024];
extern "C" void* LCGetBase() { return s_lcData; }

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            std::exit(1);                                                                          \
        }                                                                                           \
    } while (0)

// Writes a 3x4 matrix into the locked-cache buffer PSMTXReorder-style (column-major, matching
// src/game/trans.cpp's ReorderMtxToLC): dst[col*3+row] = src[row][col].
static void WriteMatrixToLC(int idx, const f32 M[3][4])
{
    f32* dst = (f32*) (s_lcData + idx * 0x30);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            dst[c * 3 + r] = M[r][c];
        }
    }
}

static int RunXCases()
{
    int failures = 0;
    for (const auto& tc : kTransSkinXCases) {
        std::memset(s_lcData, 0, sizeof(s_lcData));
        for (int i = 0; i < tc.nMat; i++) {
            WriteMatrixToLC(i, tc.mat[i]);
        }
        // Big-endian byte layout, matching real hardware's native vertex buffer format (see
        // src/port/trans_skin.cpp's LoadBE16/StoreBE16 comment) and tools/port/ppcsim's
        // struct.pack(">hhhh", ...) -- NOT a host-native s16 array.
        u8 src[8 * 8];
        for (int i = 0; i < tc.n; i++) {
            for (int k = 0; k < 4; k++) {
                u16 v = (u16) tc.verts[i][k];
                src[i * 8 + k * 2 + 0] = (u8) (v >> 8);
                src[i * 8 + k * 2 + 1] = (u8) v;
            }
        }
        u8 dst[64];
        std::memset(dst, 0, sizeof(dst));
        setupGQR6(tc.gqr6);
        CalcSk1_x(dst, src, tc.n);
        if (std::memcmp(dst, tc.expected, tc.expectedLen) != 0) {
            std::fprintf(stderr, "FAIL CalcSk1_x: gqr6=0x%08x n=%d\n", tc.gqr6, tc.n);
            for (int i = 0; i < tc.expectedLen; i++) {
                std::fprintf(stderr, "  [%d] got=0x%02x want=0x%02x%s\n", i, dst[i], tc.expected[i],
                             dst[i] != tc.expected[i] ? "  <-- MISMATCH" : "");
            }
            failures++;
        }
    }
    return failures;
}

static int RunX2Cases()
{
    int failures = 0;
    for (const auto& tc : kTransSkinX2Cases) {
        std::memset(s_lcData, 0, sizeof(s_lcData));
        for (int i = 0; i < tc.nMat; i++) {
            WriteMatrixToLC(i, tc.mat[i]);
        }
        u8 src[8 * 4];
        for (int i = 0; i < tc.n; i++) {
            src[i * 4 + 0] = (u8) tc.verts[i][0];
            src[i * 4 + 1] = (u8) tc.verts[i][1];
            src[i * 4 + 2] = (u8) tc.verts[i][2];
            src[i * 4 + 3] = (u8) tc.verts[i][3];
        }
        u8 dst[64];
        std::memset(dst, 0, sizeof(dst));
        setupGQR6(tc.gqr6);
        CalcSk1_x2(dst, src, tc.n);
        if (std::memcmp(dst, tc.expected, tc.expectedLen) != 0) {
            std::fprintf(stderr, "FAIL CalcSk1_x2: gqr6=0x%08x n=%d\n", tc.gqr6, tc.n);
            for (int i = 0; i < tc.expectedLen; i++) {
                std::fprintf(stderr, "  [%d] got=0x%02x want=0x%02x%s\n", i, dst[i], tc.expected[i],
                             dst[i] != tc.expected[i] ? "  <-- MISMATCH" : "");
            }
            failures++;
        }
    }
    return failures;
}

int main()
{
    int failures = RunXCases() + RunX2Cases();
    if (failures) {
        std::fprintf(stderr, "%d case(s) FAILED\n", failures);
        return 1;
    }
    std::printf("test_trans_skin: all %zu + %zu cases passed\n",
                sizeof(kTransSkinXCases) / sizeof(kTransSkinXCases[0]),
                sizeof(kTransSkinX2Cases) / sizeof(kTransSkinX2Cases[0]));
    return 0;
}
