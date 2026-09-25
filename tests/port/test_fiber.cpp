// Host test for src/port/fiber.cpp (docs/port-boot.md section 34). What it proves:
//  - Ping-pong: two-way control transfer between a real host thread's own context and a fiber
//    with its own stack, several times, values threaded through correctly each time.
//  - 10000-switch stress test with live NEON (float32x4_t) state on the fiber side: if
//    swapcontext() ever failed to preserve callee-saved vector register state across a switch (the
//    concrete risk include/port/fiber.h's header comment calls out and says was checked), the
//    running vector sum computed on the fiber side would desync from the value read back after
//    each switch.
#include "port/fiber.h"

#include <arm_neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using re4_port::Fiber;
using re4_port::FiberCreate;
using re4_port::FiberFromCurrentContext;
using re4_port::FiberSwitchTo;

namespace {

Fiber* g_main = nullptr;
Fiber* g_worker = nullptr;

struct PingPongState {
    int counter = 0;
};

void PingPongEntry(void* arg)
{
    auto* st = static_cast<PingPongState*>(arg);
    for (;;) {
        st->counter++;
        FiberSwitchTo(g_worker, g_main);
    }
}

struct NeonState {
    float32x4_t v;
    int iter = 0;
};

Fiber* g_neonWorker = nullptr;

void NeonEntry(void* arg)
{
    auto* st = static_cast<NeonState*>(arg);
    float32x4_t v = st->v;
    for (;;) {
        v = vaddq_f32(v, vdupq_n_f32(1.0f));
        st->v = v;
        st->iter++;
        FiberSwitchTo(g_neonWorker, g_main);
    }
}

bool Fail(const char* what)
{
    std::fprintf(stderr, "test_fiber: FAILED: %s\n", what);
    return false;
}

} // namespace

int main()
{
    g_main = FiberFromCurrentContext();

    // --- Ping-pong ---
    static std::vector<char> pingPongStack(64 * 1024);
    PingPongState pp;
    g_worker = FiberCreate(pingPongStack.data(), pingPongStack.size(), PingPongEntry, &pp);
    for (int i = 1; i <= 5; i++) {
        FiberSwitchTo(g_main, g_worker);
        if (pp.counter != i) {
            return Fail("ping-pong counter mismatch"), 1;
        }
    }
    std::printf("test_fiber: ping-pong OK (5 round trips)\n");

    // --- 10000-switch NEON stress ---
    static std::vector<char> neonStack(64 * 1024);
    NeonState ns{};
    ns.v = vdupq_n_f32(0.0f);
    g_neonWorker = FiberCreate(neonStack.data(), neonStack.size(), NeonEntry, &ns);
    constexpr int kIterations = 10000;
    for (int i = 1; i <= kIterations; i++) {
        FiberSwitchTo(g_main, g_neonWorker);
        float out[4];
        vst1q_f32(out, ns.v);
        for (float f : out) {
            if (f != static_cast<float>(i)) {
                std::fprintf(stderr, "test_fiber: NEON state corrupted at iteration %d (got %f, "
                                      "want %d)\n",
                             i, f, i);
                return 1;
            }
        }
        if (ns.iter != i) {
            return Fail("NEON iter counter mismatch"), 1;
        }
    }
    std::printf("test_fiber: %d-switch NEON stress OK\n", kIterations);

    std::printf("test_fiber: OK\n");
    return 0;
}
