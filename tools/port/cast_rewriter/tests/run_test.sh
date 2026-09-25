#!/usr/bin/env bash
# Regression test for the macro-range bug fixed in CastRewriter.cpp (docs/port-phase2.md section 8,
# cmake/boot_exclude.txt's note on the fix). Runs the real rewriter binary on
# fixture_macro_ranges.cpp, then checks the rewritten output two ways:
#   1. text patterns -- catches the exact bug found live (macro parameter names `arc`/`no` leaking
#      into the output instead of the call site's real arguments, or an unconverted trailing
#      argument list left after a truncated replacement range);
#   2. an actual compile (-fsyntax-only against a stub re4_port::GC32/GCPTR<T>) -- catches anything
#      the text patterns do not, the same way `sce_sys.cpp`'s real bug was only fully confirmed by
#      compiling the generated build-pc/gen/ copy, not just reading the rewriter's own report.
#
# Usage: run_test.sh <path-to-re4_cast_rewriter> <resource-dir>
set -euo pipefail

REWRITER="$1"
RESOURCE_DIR="$2"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIXTURE="$HERE/fixture_macro_ranges.cpp"
OUT="$(mktemp -t re4_cast_rewriter_test_XXXX).cpp"
REPORT="$(mktemp -t re4_cast_rewriter_test_report_XXXX).txt"
SYSROOT="$(xcrun --show-sdk-path)"

trap 'rm -f "$OUT" "$REPORT"' EXIT

"$REWRITER" "$FIXTURE" -o "$OUT" --report "$REPORT" --resource-dir="$RESOURCE_DIR" \
    -- -isysroot "$SYSROOT" -std=c++17 -fms-extensions -Wno-error -ferror-limit=0

fail() {
    echo "FAIL: $1" >&2
    echo "--- rewritten output ---" >&2
    cat "$OUT" >&2
    echo "--- report ---" >&2
    cat "$REPORT" >&2
    exit 1
}

# The macro's own parameter names must never leak into a *call site* as bare identifiers -- that is
# exactly the bug (`arc`/`no` from ARC_LIKE_PTR's own definition instead of the call site's real
# args). Excludes the #define line itself (ARC_LIKE_PTR's own header-lookalike text, untouched on
# purpose, legitimately contains `arc`/`no`).
grep -v '^#define ARC_LIKE_PTR' "$OUT" | grep -Eq 'ARC_LIKE_PTR\(arc, no\)|GC32\(\(void\*\) \(\(u8\*\) \(arc\)' && \
    fail "macro parameter names 'arc'/'no' leaked into a call site's rewritten output verbatim"

# Every ARC_LIKE_PTR call site must survive as a whole call (real arguments, not truncated) inside
# the GC32() wrapper -- the truncation bug left a bare, unconverted trailing '(idxA)'/'(idxB)' after
# the replacement instead.
grep -q 're4_port::GC32(ARC_LIKE_PTR(pArc, idxA))' "$OUT" || fail "ARC_LIKE_PTR(pArc, idxA) call site not composed as one unit"
grep -q 're4_port::GC32(ARC_LIKE_PTR(pArc, idxB))' "$OUT" || fail "ARC_LIKE_PTR(pArc, idxB) call site not composed as one unit"

# The object-like in-file macro's own cast must be rewritten once, at its #define (MacroBody), and
# left as an opaque call (`OBJECT_LIKE_PTR`) at the outer cast's call site, not recomposed inline
# there a second time (src/game/dvd.cpp's DVD_BUFF2 bug).
grep -q '#define OBJECT_LIKE_PTR (re4_port::GCPTR<void>((std::uint32_t)(0x80360000)))' "$OUT" || \
    fail "OBJECT_LIKE_PTR's own #define body was not rewritten to GCPTR"
grep -q 're4_port::GC32(OBJECT_LIKE_PTR)' "$OUT" || fail "outer cast at OBJECT_LIKE_PTR's call site was not rewritten"

# The function-like in-file macro's own nested cast (DATA_PTR_LIKE's inner `(u32) (d)`) must be
# composed into its #define body once -- the call site itself is untouched (not a cast).
grep -q '#define DATA_PTR_LIKE(d, ofs) (re4_port::GCPTR<void>((std::uint32_t)((\*(u32\*) ((u8\*) (d) + (ofs)) + (u32)re4_port::GC32((d))))))' "$OUT" || \
    fail "DATA_PTR_LIKE's own #define body was not rewritten with its nested cast composed in"

# And the whole thing must actually compile against a stub re4_port::GC32/GCPTR<T> -- the strongest
# check, catches anything the text patterns above do not.
/usr/bin/c++ -std=c++17 -include "$HERE/re4_port_stub.h" -fsyntax-only "$OUT" || \
    fail "rewritten output does not compile"

echo "PASS"
