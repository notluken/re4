#!/usr/bin/env python3
"""Cross-checks CalcSk1_x/CalcSk1_x2: the vendor's whole-function PPC asm (executed here by
tools/port/ppcsim's generic instruction interpreter) against the closed-form C-equivalent
formula documented in calc_sk1.py's own docstring, on randomized synthetic inputs (fixed seed)
plus edge cases (zero weights handled upstream in MakeWeightPalette, not here; max s16, negative
values, matrix index 0 and a high index).

Run directly: `python3 tools/port/ppcsim/test_calc_sk1.py`. Also regenerates
`tests/port/trans_skin_vectors.h` (deterministic, fixed seed) -- the same reference vectors the
C++ host test (`tests/port/test_trans_skin.cpp`) compares the actual `src/game/trans.cpp` port
against, so the C port is checked against an interpreter-derived reference, not just against its
own derivation typed twice.
"""
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ppcsim import calc_sk1, gqr

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        print(f"FAIL: {msg}")
        failures += 1


def test_asm_text_matches_source():
    src_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                             "src", "game", "trans.cpp")
    with open(src_path) as f:
        src = f.read()

    def norm(asm_text):
        return "\n".join(l.strip() for l in asm_text.strip("\n").split("\n") if l.strip())

    for name, asm in (("CalcSk1_x", calc_sk1.CALC_SK1_X_ASM), ("CalcSk1_x2", calc_sk1.CALC_SK1_X2_ASM)):
        # Pull the literal asm volatile(...) block that follows `void <name>(...)` from the source.
        start = src.rindex(f"void {name}(")
        block_start = src.index('asm volatile(', start)
        block_end = src.index(');', block_start)
        block = src[block_start:block_end]
        # Extract the quoted instruction lines.
        lines = []
        for m in block.split('"'):
            m = m.strip()
            if m and m not in ("", "\\n"):
                if m.endswith("\\n"):
                    lines.append(m[:-2].strip())
        got = "\n".join(lines)
        want = norm(asm)
        check(got == want, f"{name}: copied asm text differs from src/game/trans.cpp\n--- copy ---\n{want}\n--- source ---\n{got}")


test_asm_text_matches_source()


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s8(v):
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


def rand_matrix(rng):
    return [[rng.uniform(-2.0, 2.0) for _ in range(4)] for _ in range(3)]


def run_case_x(rng, n, gqr6, verts, matrices, dump=None):
    ld_type, ld_scale, st_type, st_scale = gqr.gqr_fields(gqr6)

    def write_lc(writer):
        for idx, M in matrices.items():
            calc_sk1.write_matrix(writer, idx, M)

    src = b""
    for (x, y, z, idx) in verts:
        src += struct.pack(">hhhh", x, y, z, idx)
    dst_init = bytes(n * 6)
    got = calc_sk1.run_calc_sk1_x(write_lc, dst_init, src, n, gqr6)

    want = b""
    for (x, y, z, idx) in verts:
        M = matrices[idx]
        fx = gqr.dequantize(x, ld_type, ld_scale)
        fy = gqr.dequantize(y, ld_type, ld_scale)
        fz = gqr.dequantize(z, ld_type, ld_scale)
        out = calc_sk1.closed_form(M, fx, fy, fz)
        want += struct.pack(">hhh", gqr.quantize(out[0], st_type, st_scale),
                             gqr.quantize(out[1], st_type, st_scale),
                             gqr.quantize(out[2], st_type, st_scale))
    check(got == want, f"CalcSk1_x mismatch: got={got.hex()} want={want.hex()}")
    if dump is not None:
        dump.append((gqr6, matrices, verts, got))
    return got == want


def run_case_x2(rng, n, gqr6, verts, matrices, dump=None):
    ld_type, ld_scale, st_type, st_scale = gqr.gqr_fields(gqr6)

    def write_lc(writer):
        for idx, M in matrices.items():
            calc_sk1.write_matrix(writer, idx, M)

    src = b""
    for (x, y, z, idx) in verts:
        src += struct.pack(">bbbB", x, y, z, idx)
    dst_init = bytes(n * 3)
    got = calc_sk1.run_calc_sk1_x2(write_lc, dst_init, src, n, gqr6)

    want = b""
    for (x, y, z, idx) in verts:
        M = matrices[idx]
        fx = gqr.dequantize(x, ld_type, ld_scale)
        fy = gqr.dequantize(y, ld_type, ld_scale)
        fz = gqr.dequantize(z, ld_type, ld_scale)
        out = calc_sk1.closed_form(M, fx, fy, fz)
        want += struct.pack(">bbb", gqr.quantize(out[0], st_type, st_scale),
                             gqr.quantize(out[1], st_type, st_scale),
                             gqr.quantize(out[2], st_type, st_scale))
    check(got == want, f"CalcSk1_x2 mismatch: got={got.hex()} want={want.hex()}")
    if dump is not None:
        dump.append((gqr6, matrices, verts, got))
    return got == want


rng = random.Random(0xC0FFEE)
dump_x = []
dump_x2 = []

# GQR6 values actually used by src/game/trans.cpp (docs/port-boot.md section 45): the
# shift-based position value (here with a representative shift of 6), the fixed default-normal
# value, and the extended (s8) normal value.
GQR6_VALUES_X = [0x00070007 | (6 << 24) | (6 << 8), 0x32073207]
GQR6_VALUES_X2 = [0x20062006]

for gqr6 in GQR6_VALUES_X:
    for trial in range(20):
        n = rng.randint(1, 6)
        matrices = {i: rand_matrix(rng) for i in range(4)}
        verts = [(rng.randint(-32768, 32767), rng.randint(-32768, 32767),
                  rng.randint(-32768, 32767), rng.randint(0, 3)) for _ in range(n)]
        run_case_x(rng, n, gqr6, verts, matrices, dump_x if trial < 3 else None)

# Edge cases: max/min s16, matrix index 0, a single vertex.
run_case_x(rng, 1, GQR6_VALUES_X[1], [(32767, -32768, 0, 0)], {0: rand_matrix(rng)}, dump_x)
run_case_x(rng, 1, GQR6_VALUES_X[1], [(0, 0, 0, 0)], {0: [[1, 0, 0, 5], [0, 1, 0, -5], [0, 0, 1, 0]]}, dump_x)

for gqr6 in GQR6_VALUES_X2:
    for trial in range(20):
        n = rng.randint(1, 6)
        matrices = {i: rand_matrix(rng) for i in range(4)}
        verts = [(rng.randint(-128, 127), rng.randint(-128, 127),
                  rng.randint(-128, 127), rng.randint(0, 3)) for _ in range(n)]
        run_case_x2(rng, n, gqr6, verts, matrices, dump_x2 if trial < 3 else None)

run_case_x2(rng, 1, GQR6_VALUES_X2[0], [(127, -128, 0, 0)], {0: rand_matrix(rng)}, dump_x2)


def fmt_f(v):
    s = f"{v:.9g}"
    if "." not in s and "e" not in s and "E" not in s and "inf" not in s and "nan" not in s:
        s += ".0"
    return s + "f"


def emit_header():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                         "tests", "port", "trans_skin_vectors.h")
    lines = []
    lines.append("// GENERATED by tools/port/ppcsim/test_calc_sk1.py -- do not hand-edit.")
    lines.append("// Reference vectors for CalcSk1_x/CalcSk1_x2's TARGET_PC C port, produced by")
    lines.append("// running the vendor's own asm (src/game/trans.cpp) through")
    lines.append("// tools/port/ppcsim's PowerPC interpreter on synthetic (non-disc) inputs.")
    lines.append("#pragma once")
    lines.append('#include "types.h"')
    lines.append("")
    lines.append("struct TransSkinCase {")
    lines.append("    u32 gqr6;")
    lines.append("    int n;")
    lines.append("    int nMat;")
    lines.append("    f32 mat[4][3][4];")
    lines.append("    s16 verts[8][4];")
    lines.append("    u8 expected[64];")
    lines.append("    int expectedLen;")
    lines.append("};")
    lines.append("")
    lines.append("static const TransSkinCase kTransSkinXCases[] = {")
    for gqr6, matrices, verts, expected in dump_x:
        lines.append("    {")
        lines.append(f"        0x{gqr6:08x}u, {len(verts)}, {len(matrices)},")
        lines.append("        {")
        for i in range(4):
            M = matrices.get(i, [[0, 0, 0, 0]] * 3)
            rows = ", ".join("{" + ", ".join(fmt_f(v) for v in row) + "}" for row in M)
            lines.append(f"            {{{rows}}},")
        lines.append("        },")
        vlines = ", ".join("{%d,%d,%d,%d}" % v for v in verts)
        pad = ", ".join(["{0,0,0,0}"] * (8 - len(verts)))
        sep = ", " if pad else ""
        lines.append(f"        {{{vlines}{sep}{pad}}},")
        hexbytes = ", ".join(f"0x{b:02x}" for b in expected)
        padb = ", ".join(["0x00"] * (64 - len(expected)))
        sepb = ", " if padb else ""
        lines.append(f"        {{{hexbytes}{sepb}{padb}}}, {len(expected)}")
        lines.append("    },")
    lines.append("};")
    lines.append("")
    lines.append("static const TransSkinCase kTransSkinX2Cases[] = {")
    for gqr6, matrices, verts, expected in dump_x2:
        lines.append("    {")
        lines.append(f"        0x{gqr6:08x}u, {len(verts)}, {len(matrices)},")
        lines.append("        {")
        for i in range(4):
            M = matrices.get(i, [[0, 0, 0, 0]] * 3)
            rows = ", ".join("{" + ", ".join(fmt_f(v) for v in row) + "}" for row in M)
            lines.append(f"            {{{rows}}},")
        lines.append("        },")
        vlines = ", ".join("{%d,%d,%d,%d}" % v for v in verts)
        pad = ", ".join(["{0,0,0,0}"] * (8 - len(verts)))
        sep = ", " if pad else ""
        lines.append(f"        {{{vlines}{sep}{pad}}},")
        hexbytes = ", ".join(f"0x{b:02x}" for b in expected)
        padb = ", ".join(["0x00"] * (64 - len(expected)))
        sepb = ", " if padb else ""
        lines.append(f"        {{{hexbytes}{sepb}{padb}}}, {len(expected)}")
        lines.append("    },")
    lines.append("};")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {path}")


if failures:
    print(f"{failures} FAILED")
    sys.exit(1)

emit_header()
print("test_calc_sk1: all checks passed")
