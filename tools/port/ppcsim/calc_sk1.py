"""Runs src/game/trans.cpp's CalcSk1_x / CalcSk1_x2 -- the vendor's own `asm volatile` text,
copied verbatim below, byte for byte, from the source file (checked by
`test_calc_sk1.py::test_asm_text_matches_source`) -- through tools/port/ppcsim's generic
instruction interpreter, on synthetic (non-disc) inputs, and cross-checks the result against a
closed-form C-equivalent (the operation this project's `docs/port-boot.md` section 45 derived by
hand, instruction by instruction): for vertex v=(x,y,z,1) and the ROMtx `M` at
LCGetBase()+idx*0x30 (12 floats, column-major -- M[col*3+row], i.e. M[0..2] is column 0 etc, the
layout `PSMTXReorder` produces), row r of the plain 3x4 affine transform is

    out[r] = fma(M[6+r], z, fma(M[3+r], y, fma(M[r], x, M[9+r])))

(three chained fused multiply-adds, matching the hardware's own `ps_madds0`/`ps_madds1` chain
exactly -- not `M[r]*x + M[3+r]*y + M[6+r]*z + M[9+r]` evaluated with unspecified rounding/
contraction). This module exists so a from-scratch C port can be checked against an
independently-obtained reference for the same random inputs, not just against this same
derivation typed twice.
"""
import struct

from . import gqr
from .sim import Interpreter, Memory

# Copied verbatim from src/game/trans.cpp's CalcSk1_x/CalcSk1_x2 (grep -A17 in the source to
# re-verify after any edit there).
CALC_SK1_X_ASM = """
lha 9, 6(4)
subi 3, 3, 6
li 0, 0
subi 4, 4, 8
mulli 9, 9, 0x30
addis 9, 9, 0xE000
mtctr 5
1:
psq_l 0, 0(9), 0, 0
psq_l 1, 8(9), 1, 0
psq_l 2, 0xc(9), 0, 0
psq_l 3, 0x14(9), 1, 0
psq_l 4, 0x18(9), 0, 0
psq_l 5, 0x20(9), 1, 0
psq_l 6, 0x24(9), 0, 0
psq_l 7, 0x2c(9), 1, 0
psq_lu 8, 8(4), 0, 6
psq_l 9, 4(4), 1, 6
ps_madds0 12, 0, 8, 6
ps_madds0 13, 1, 8, 7
ps_madds1 12, 2, 8, 12
ps_madds1 13, 3, 8, 13
ps_madds0 12, 4, 9, 12
ps_madds0 13, 5, 9, 13
psq_stu 12, 6(3), 0, 6
psq_st 13, 4(3), 1, 6
lha 9, 0xe(4)
mulli 9, 9, 0x30
addis 9, 9, 0xE000
bdnz 1b
"""

CALC_SK1_X2_ASM = """
lbz 9, 3(4)
subi 3, 3, 3
li 0, 0
subi 4, 4, 4
mulli 9, 9, 0x30
addis 9, 9, 0xE000
mtctr 5
1:
psq_l 0, 0(9), 0, 0
psq_l 1, 8(9), 1, 0
psq_l 2, 0xc(9), 0, 0
psq_l 3, 0x14(9), 1, 0
psq_l 4, 0x18(9), 0, 0
psq_l 5, 0x20(9), 1, 0
psq_l 6, 0x24(9), 0, 0
psq_l 7, 0x2c(9), 1, 0
psq_lu 8, 4(4), 0, 6
psq_l 9, 2(4), 1, 6
ps_madds0 12, 0, 8, 6
ps_madds0 13, 1, 8, 7
ps_madds1 12, 2, 8, 12
ps_madds1 13, 3, 8, 13
ps_madds0 12, 4, 9, 12
ps_madds0 13, 5, 9, 13
psq_stu 12, 3(3), 0, 6
psq_st 13, 2(3), 1, 6
lbz 9, 7(4)
mulli 9, 9, 0x30
addis 9, 9, 0xE000
bdnz 1b
"""

LC_BASE = 0xE0000000
LC_SIZE = 16 * 1024  # matches Aurora's real LCGetBase() buffer size


def _split(asm_text):
    lines = [l.strip() for l in asm_text.strip("\n").split("\n") if l.strip()]
    assert lines[7] == "1:", lines[7]
    preamble = lines[:7]
    body = lines[8:-1]  # between "1:" and "bdnz 1b"
    assert lines[-1] == "bdnz 1b", lines[-1]
    return preamble, body


_PRE_X, _BODY_X = _split(CALC_SK1_X_ASM)
_PRE_X2, _BODY_X2 = _split(CALC_SK1_X2_ASM)


def _run(preamble, body, dst_addr, src_addr, n, gqr6, dst_mem, src_mem):
    mem = Memory()
    mem.add_region(LC_BASE, LC_SIZE)
    mem.add_region(dst_addr, len(dst_mem))
    mem.add_region(src_addr, len(src_mem))
    mem.write(dst_addr, dst_mem)
    mem.write(src_addr, src_mem)
    interp = Interpreter(mem)
    interp.gpr[3] = dst_addr
    interp.gpr[4] = src_addr
    interp.gpr[5] = n
    interp.spr[912 + 6] = gqr6
    for line in preamble:
        interp.step(line)
    for _ in range(n):
        for line in body:
            interp.step(line)
    return bytes(mem.read(dst_addr, len(dst_mem)))


def write_matrix(mem_writer, idx, M):
    """M: 3x4 nested list, M[row][col]. Writes it PSMTXReorder-style (column-major, 12 floats,
    ROMtx layout: M_flat[col*3+row]) at LC_BASE + idx*0x30, exactly what MakeWeightPalette /
    MakeWeightPaletteExt's real `PSMTXReorder(m, target)` call produces."""
    flat = [0.0] * 12
    for row in range(3):
        for col in range(4):
            flat[col * 3 + row] = M[row][col]
    mem_writer(LC_BASE + idx * 0x30, b"".join(struct.pack(">f", v) for v in flat))


def closed_form(M, x, y, z):
    def fma(a, b, c):
        import math
        return math.fma(a, b, c) if hasattr(math, "fma") else a * b + c

    out = [0.0, 0.0, 0.0]
    for r in range(3):
        out[r] = fma(M[r][2], z, fma(M[r][1], y, fma(M[r][0], x, M[r][3])))
    return out


def run_calc_sk1_x(mem_write_lc, dst_bytes, src_bytes, n, gqr6):
    mem = Memory()
    mem.add_region(LC_BASE, LC_SIZE)
    dst_addr = 0x10000000
    src_addr = 0x20000000
    mem.add_region(dst_addr, len(dst_bytes))
    mem.add_region(src_addr, len(src_bytes) + 8)  # +8: the loop's own one-past-end prefetch read
    mem.write(dst_addr, dst_bytes)
    mem.write(src_addr, src_bytes)
    mem_write_lc(mem.write)
    interp = Interpreter(mem)
    interp.gpr[3] = dst_addr
    interp.gpr[4] = src_addr
    interp.gpr[5] = n
    interp.spr[912 + 6] = gqr6
    for line in _PRE_X:
        interp.step(line)
    for _ in range(n):
        for line in _BODY_X:
            interp.step(line)
    return bytes(mem.read(dst_addr, len(dst_bytes)))


def run_calc_sk1_x2(mem_write_lc, dst_bytes, src_bytes, n, gqr6):
    mem = Memory()
    mem.add_region(LC_BASE, LC_SIZE)
    dst_addr = 0x10000000
    src_addr = 0x20000000
    mem.add_region(dst_addr, len(dst_bytes))
    mem.add_region(src_addr, len(src_bytes) + 4)
    mem.write(dst_addr, dst_bytes)
    mem.write(src_addr, src_bytes)
    mem_write_lc(mem.write)
    interp = Interpreter(mem)
    interp.gpr[3] = dst_addr
    interp.gpr[4] = src_addr
    interp.gpr[5] = n
    interp.spr[912 + 6] = gqr6
    for line in _PRE_X2:
        interp.step(line)
    for _ in range(n):
        for line in _BODY_X2:
            interp.step(line)
    return bytes(mem.read(dst_addr, len(dst_bytes)))
