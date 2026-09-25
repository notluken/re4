// Regression fixture for the re4_cast_rewriter macro-range bug (see CastRewriter.cpp's comments on
// getExpansionRange vs getExpansionLoc, and cmake/boot_exclude.txt's note on the fix). Deliberately
// self-contained (no project headers) so the test can run the real tool standalone, with a fixed
// `--` compilation database, no compile_commands.json lookup needed.
//
// Three shapes, matching the real bugs found in src/game/objRocket.cpp, src/game/dvd.cpp and
// src/game/mercenaries.cpp:

typedef unsigned int u32;
typedef unsigned char u8;

// (a) header_macro.h-lookalike: a function-like macro whose body casts a pointer to void*, no cast
// of its own kind that needs rewriting (mirrors global.h's ARC_PTR-family under TARGET_PC: plain
// pointer arithmetic, not itself a target for this tool). Its own body is included here as if from
// a header (spelling location NOT in this main file) so the tool must never touch it, and every
// call site's *outer* cast (added below, written directly in this main file) is the only thing that
// should be rewritten.
#define ARC_LIKE_PTR(arc, no) ((void*) ((u8*) (arc) + (arc)->ofs[no]))

struct Arc {
    u32 ofs[4];
};

// (b) an object-like macro #define'd in THIS file (the "MacroBody" case, like dvd.cpp's DVD_BUFF2):
// its own IntegralToPointer cast must be rewritten once, at the #define's own text, not re-inlined
// at every call site.
#define OBJECT_LIKE_PTR ((void*) 0x80360000)

// (c) a function-like macro #define'd in THIS file whose body itself contains TWO qualifying casts
// (an outer IntegralToPointer wrapping an inner PointerToIntegral), like mercenaries.cpp's
// DATA_PTR -- the inner cast must still be composed into the outer's rewritten macro-body text,
// since its own independent visit gets skipped as "already inside the outer's rewritten range".
#define DATA_PTR_LIKE(d, ofs) ((void*) (*(u32*) ((u8*) (d) + (ofs)) + (u32) (d)))

// Deliberately NOT named `arc`/`no` (the macro's own parameter names): a rewritten call site that
// leaked the macro's #define-parameter spelling instead of substituting the real arguments would
// otherwise be invisible to a text-based check that only greps for identifiers already used here.
void useArcLike(Arc* pArc, int idxA, int idxB)
{
    // (a): outer (u32) cast written directly here, argument is the function-like header macro call
    // -- after the fix, the whole `(u32) ARC_LIKE_PTR(pArc, idxA)` must be replaced as one unit,
    // with the real arguments (`pArc`, `idxA`/`idxB`) preserved and the macro's own parameter names
    // (`arc`, `no`) never appearing literally in the output.
    u32 a = (u32) ARC_LIKE_PTR(pArc, idxA);
    u32 b = (u32) ARC_LIKE_PTR(pArc, idxB);
    (void) a;
    (void) b;
}

void useObjectLike()
{
    // (b): OBJECT_LIKE_PTR used as the argument of an outer (u32) cast written directly here.
    u32 v = (u32) OBJECT_LIKE_PTR;
    (void) v;
}

// Deliberately NOT named `d`/`ofs` (DATA_PTR_LIKE's own parameter names), same reasoning as
// useArcLike above.
void useDataPtrLike(void* pData, u32 byteOfs)
{
    // (c): a direct call to the function-like macro whose OWN body has a nested cast.
    void* p = DATA_PTR_LIKE(pData, byteOfs);
    (void) p;
}

// (d) src/game/read.cpp's real PL_DATA_ADDR/DVD_READ_N bug: a function-like macro (READ_N_LIKE,
// like DVD_READ_N) invoked with an argument that is itself an IntegralToPointer cast of a plain
// object-like macro (FIXED_ADDR_LIKE, like PL_DATA_ADDR -- no cast of its own at the #define, so
// it is NOT a "MacroBody" rewrite site the way OBJECT_LIKE_PTR above is). The cast's own ExprLoc is
// then a macro-ARGUMENT location (isMacroID() true), which must not be confused with a cast
// spelled inside a macro's own definition body: doing so chases getSpellingLoc(EditEnd) through
// FIXED_ADDR_LIKE's *own* #define text (a different, earlier point in this file) instead of
// stopping at this call site, producing a nonsensical reversed edit range that the tool then
// silently drops as "already rewritten" -- leaving the original, unconverted `(void*)
// FIXED_ADDR_LIKE` in the output. Real-world consequence (read.cpp): a GameCube fixed-MEM1-address
// literal handed to a host function verbatim, instead of through GCPTR -- a real host-memory crash.
#define FIXED_ADDR_LIKE 0x807EC000
#define READ_N_LIKE(dst, mode) doReadLike(dst, mode)

int doReadLike(void* dst, int mode);

void useFixedAddrAsMacroArg()
{
    int r = READ_N_LIKE((void*) FIXED_ADDR_LIKE, 0x8100);
    (void) r;
}
