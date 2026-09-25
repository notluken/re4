// See include/port/fiber.h for the design note (why ucontext, verified working on this host).
#ifndef TARGET_PC
#error "src/port/fiber.cpp is host-only (TARGET_PC)"
#endif

#define _XOPEN_SOURCE 1 // macOS's <ucontext.h> #errors without this (its deprecated-routines
                         // guard) -- see include/port/fiber.h's header comment for why the
                         // deprecation itself does not apply to this use.
#include "port/fiber.h"

#include <cstdio>
#include <cstdlib>
#include <ucontext.h>

namespace re4_port {

struct Fiber {
    ucontext_t ctx{};
    FiberEntry entry = nullptr;
    void* arg = nullptr;
    void (*on_finish)(Fiber*, void*) = nullptr;
    bool is_context_wrapper = false; // FiberFromCurrentContext() result: no stack of our own
};

namespace {

thread_local Fiber* t_current = nullptr;

// makecontext() only portably accepts `int` varargs, not a pointer -- the standard workaround
// (also used by, e.g., Boost.Context's ucontext backend) is to split a 64-bit pointer into two
// 32-bit halves and reassemble them in the trampoline.
void FiberTrampoline(unsigned hi, unsigned lo)
{
    auto ptr = (static_cast<std::uintptr_t>(hi) << 32) | static_cast<std::uintptr_t>(lo);
    Fiber* self = reinterpret_cast<Fiber*>(ptr);
    self->entry(self->arg);
    if (self->on_finish) {
        self->on_finish(self, self->arg);
        std::fprintf(stderr, "re4_port: Fiber %p's on_finish() returned -- programming error, "
                              "there is no valid context left to resume, aborting\n",
                     (void*) self);
        std::abort();
    }
    std::fprintf(stderr, "re4_port: Fiber %p's entry() returned with no on_finish -- programming "
                          "error, aborting\n",
                 (void*) self);
    std::abort();
}

} // namespace

Fiber* FiberFromCurrentContext()
{
    auto* f = new Fiber();
    f->is_context_wrapper = true;
    t_current = f;
    return f;
}

Fiber* FiberCreate(void* stack_base, std::size_t stack_size, FiberEntry entry, void* arg,
                    void (*on_finish)(Fiber*, void*))
{
    auto* f = new Fiber();
    f->entry = entry;
    f->arg = arg;
    f->on_finish = on_finish;
    if (getcontext(&f->ctx) != 0) {
        std::fprintf(stderr, "re4_port: FiberCreate: getcontext() failed\n");
        std::abort();
    }
    f->ctx.uc_stack.ss_sp = stack_base;
    f->ctx.uc_stack.ss_size = stack_size;
    f->ctx.uc_link = nullptr; // entry() must never return through this path -- see FiberTrampoline
    auto ptr = reinterpret_cast<std::uintptr_t>(f);
    makecontext(&f->ctx, reinterpret_cast<void (*)()>(FiberTrampoline), 2,
                static_cast<unsigned>(ptr >> 32), static_cast<unsigned>(ptr & 0xffffffffu));
    return f;
}

void FiberSwitchTo(Fiber* from, Fiber* to)
{
    t_current = to;
    if (swapcontext(&from->ctx, &to->ctx) != 0) {
        std::fprintf(stderr, "re4_port: FiberSwitchTo: swapcontext() failed\n");
        std::abort();
    }
}

Fiber* FiberCurrent()
{
    return t_current;
}

void FiberDestroy(Fiber* f)
{
    delete f;
}

} // namespace re4_port
