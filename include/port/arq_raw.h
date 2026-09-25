// TARGET_PC-only helper for the ARQCallback boundary (see include/dolphin/ar.h's ARQCallback
// comment, src/game/dvd.cpp's trans2aram_cb): Aurora's real ARQPostRequest
// (../aurora/lib/dolphin/AR.cpp) hands its callback a genuine, full-width host pointer, not this
// port's usual GameCube-looking compressed handle (re4_port::GC32()/GCPTR(), include/port/
// ptr32.h) -- reinterpreting it needs a plain function, never a cast, so the build's cast rewriter
// (tools/port/cast_rewriter, which only ever rewrites cast expressions) never "corrects" it back
// into a wrong GCPTR() translation.
#ifndef TARGET_PC
#error "include/port/arq_raw.h is host-only (TARGET_PC)"
#endif

#ifndef RE4_PORT_ARQ_RAW_H
#define RE4_PORT_ARQ_RAW_H

#include <cstring>

namespace re4_port {

// A plain memcpy-based type pun, not a cast: unlike a real `reinterpret_cast`/C-style cast, this
// has no cast expression anywhere in it for the build's cast-rewriter (tools/port/cast_rewriter)
// to find and "correct" back into a re4_port::GCPTR() translation -- confirmed live it does so
// even inside a helper function's own body, wherever the cast textually appears in the
// translation unit, not just at a game-code call site (docs/port-boot.md, `trans2aram_cb`).
template <class T>
inline T* RawArqPtr(unsigned long v)
{
    T* p;
    std::memcpy(&p, &v, sizeof(p));
    return p;
}

} // namespace re4_port

#endif // RE4_PORT_ARQ_RAW_H
