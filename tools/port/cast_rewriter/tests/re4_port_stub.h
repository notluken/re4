// Stand-in for include/port/ptr32.h's re4_port::GC32/GCPTR<T>, just enough to let the rewriter
// tool's fixture output actually compile (not just pattern-match) -- see run_test.sh.
#pragma once
#include <cstdint>

namespace re4_port {
template <class T> T* GCPTR(std::uint32_t v) { return (T*) (std::uintptr_t) v; }
inline std::uint32_t GC32(const void* p) { return (std::uint32_t) (std::uintptr_t) p; }
} // namespace re4_port
