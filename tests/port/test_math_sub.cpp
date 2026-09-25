// Host unit test for src/game/math_sub.cpp's TARGET_PC substitutions (SQRTF/SINF/COSF/
// LIMIT_ANGLE) -- docs/port-boot.md section 41's follow-up. Not testing re4_port itself (no
// include/port/ header here): links straight against src/game/math_sub.cpp, same style as the
// other tests/port/ files but for a src/game unit, since that is what changed. Compiled without
// RE4_U32_32 or the arena/Ptr32 machinery -- math_sub.cpp's four functions under test do not touch
// any on-disc/relocated field, only plain f32 arithmetic, so none of that is needed to exercise
// them.
#include "math_sub.h"
#include "db_log.h"
#include "main_mem.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

// math_sub.cpp's OTHER functions (not under test here -- everything except SQRTF/SINF/COSF/
// LIMIT_ANGLE) reference these; linking the real .o wholesale needs them resolved even though this
// test never calls the functions that use them. Minimal stand-ins, never actually invoked (each
// aborts loudly if it ever is, so a future test that exercises more of math_sub.cpp finds out
// immediately rather than silently getting zeroed vectors).
extern "C" {
void PSMTXIdentity(Mtx) { std::abort(); }
void PSMTXConcat(const Mtx, const Mtx, Mtx) { std::abort(); }
void PSMTXTranspose(const Mtx, Mtx) { std::abort(); }
u32 PSMTXInverse(const Mtx, Mtx) { std::abort(); }
void PSMTXRotRad(Mtx, char, f32) { std::abort(); }
void PSMTXRotAxisRad(Mtx, const Vec*, f32) { std::abort(); }
void PSMTXTrans(Mtx, f32, f32, f32) { std::abort(); }
void PSMTXMultVecSR(const Mtx, const Vec*, Vec*) { std::abort(); }
void PSVECAdd(const Vec*, const Vec*, Vec*) { std::abort(); }
void PSVECSubtract(const Vec*, const Vec*, Vec*) { std::abort(); }
void PSVECScale(const Vec*, Vec*, f32) { std::abort(); }
void PSVECNormalize(const Vec*, Vec*) { std::abort(); }
f32 PSVECMag(const Vec*) { std::abort(); }
f32 PSVECDotProduct(const Vec*, const Vec*) { std::abort(); }
void PSVECCrossProduct(const Vec*, const Vec*, Vec*) { std::abort(); }
}
void* mem_alloc(u32, const char*, int, int, int) { std::abort(); }
void Mem_free(void*) { std::abort(); }
void cLog::err(int, int, const char*, ...) { std::abort(); }
cLogPtr pLog = {nullptr};

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            std::exit(1);                                                                          \
        }                                                                                           \
    } while (0)

static bool near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }

int main()
{
    // SQRTF: matches std::sqrt to float precision across several magnitudes, and the vendor's own
    // "<= 0.00001f -> 0" branch (untouched by the TARGET_PC substitution, still C, not asm).
    CHECK(SQRTF(0.0f) == 0.0f);
    CHECK(SQRTF(0.000001f) == 0.0f); // below the vendor's own threshold
    CHECK(near(SQRTF(4.0f), 2.0f, 1e-5f));
    CHECK(near(SQRTF(2.0f), std::sqrt(2.0f), 1e-5f));
    CHECK(near(SQRTF(1.0e6f), std::sqrt(1.0e6f), 1e-1f));
    CHECK(near(SQRTF(0.25f), 0.5f, 1e-6f));

    // LIMIT_ANGLE: already-in-range values pass through; values outside [-PI, PI) wrap by exact
    // multiples of PI2 (branch-for-branch transcription of the vendor's asm, checked directly
    // rather than just via SINF/COSF's periodicity).
    CHECK(near(LIMIT_ANGLE(0.0f), 0.0f, 1e-6f));
    CHECK(near(LIMIT_ANGLE(1.0f), 1.0f, 1e-6f));
    CHECK(near(LIMIT_ANGLE(-PI), -PI, 1e-5f)); // -PI is in-range (>= min) by the vendor's own compare
    CHECK(near(LIMIT_ANGLE(PI), -PI, 1e-4f));  // PI is out of range (>= max) -- wraps to -PI
    CHECK(near(LIMIT_ANGLE(PI + 1.0f), -PI + 1.0f, 1e-4f));
    CHECK(near(LIMIT_ANGLE(-PI - 1.0f), PI - 1.0f, 1e-4f));
    CHECK(near(LIMIT_ANGLE(PI2 * 3.0f + 0.5f), 0.5f, 1e-3f)); // several wraps

    // SINF/COSF: agree with libm on the wrapped range, and SINF(x)^2+COSF(x)^2 == 1 (the
    // identity any sin/cos substitution must hold regardless of which polynomial computed it).
    for (f32 x = -10.0f; x <= 10.0f; x += 0.37f) {
        f32 s = SINF(x);
        f32 c = COSF(x);
        f32 wrapped = LIMIT_ANGLE(x);
        CHECK(near(s, std::sin(wrapped), 1e-4f));
        CHECK(near(c, std::cos(wrapped), 1e-4f));
        CHECK(near(s * s + c * c, 1.0f, 1e-3f));
    }

    std::printf("test_math_sub: all checks passed\n");
    return 0;
}
