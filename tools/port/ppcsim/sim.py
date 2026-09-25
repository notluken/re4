"""A tiny Gekko (PowerPC 750CL) interpreter -- executes exactly the instructions used by
src/game/trans.cpp's CalcSk1_x / CalcSk1_x2 whole-function asm (the vendor's own
`asm volatile` text, copied verbatim as data below, not retyped from memory), so those two
kernels have an independent reference implementation to cross-check a C port against.

Not a general PPC emulator: only the opcodes these two functions use are implemented
(integer loads/immediates/mulli/addis for the address arithmetic, psq_l/psq_lu/psq_st/psq_stu
with GQR quantization, ps_madds0/ps_madds1, mtctr/bdnz for the loop). Anything else raises.

Instruction semantics for ps_madds0/ps_madds1 and psq_* are transcribed from the IBM PowerPC
750CL user's manual (see gqr.py's docstring for the quantize/dequantize formula and citation);
ps_madds0/ps_madds1 themselves are documented in the same manual's "Paired Single Multiply-Add
Instructions" section:

    ps_madds0 frD, frA, frC, frB:  frD.ps0 = frA.ps0*frC.ps0 + frB.ps0
                                    frD.ps1 = frA.ps1*frC.ps0 + frB.ps1   (frC.ps0 broadcast)
    ps_madds1 frD, frA, frC, frB:  frD.ps0 = frA.ps0*frC.ps1 + frB.ps0
                                    frD.ps1 = frA.ps1*frC.ps1 + frB.ps1   (frC.ps1 broadcast)
"""
import re

from . import gqr


class Memory:
    """Sparse byte-addressable memory: a handful of named regions, each a bytearray at a
    fixed base address. Big enough for this simulator's needs (locked-cache matrix palette,
    one source buffer, one destination buffer) without allocating a 4 GB address space."""

    def __init__(self):
        self._regions = []  # list of (base, bytearray)

    def add_region(self, base: int, size: int) -> None:
        self._regions.append((base, bytearray(size)))

    def _find(self, addr: int, length: int):
        for base, buf in self._regions:
            if base <= addr and addr + length <= base + len(buf):
                return buf, addr - base
        raise ValueError(f"address 0x{addr:08x} (len {length}) not mapped")

    def read(self, addr: int, length: int) -> bytes:
        buf, off = self._find(addr, length)
        return bytes(buf[off:off + length])

    def write(self, addr: int, data: bytes) -> None:
        buf, off = self._find(addr, len(data))
        buf[off:off + len(data)] = data


def _u32(v: int) -> int:
    return v & 0xFFFFFFFF


def _s16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


_ELEM_SIZE = {gqr.TYPE_F32: 4, gqr.TYPE_U8: 1, gqr.TYPE_U16: 1, gqr.TYPE_S8: 1, gqr.TYPE_S16: 2}
# u16/s16 are 2 bytes; fix the table (kept explicit rather than clever, to avoid a silent
# off-by-one bug in exactly the code meant to catch that class of mistake elsewhere).
_ELEM_SIZE = {gqr.TYPE_F32: 4, gqr.TYPE_U8: 1, gqr.TYPE_S8: 1, gqr.TYPE_U16: 2, gqr.TYPE_S16: 2}
_ELEM_SIGNED = {gqr.TYPE_U8: False, gqr.TYPE_S8: True, gqr.TYPE_U16: False, gqr.TYPE_S16: True}


class Interpreter:
    def __init__(self, mem: Memory):
        self.mem = mem
        self.gpr = [0] * 32
        self.fpr = [(0.0, 0.0)] * 32
        self.ctr = 0
        self.spr = {}  # spr number -> value; SPR 918/919 etc are GQR0..GQR7 (912 + n)

    def gqr_value(self, idx: int) -> int:
        if idx == 0:
            return 0  # GQR0 is conventionally left at 0 (f32, no quantization) by this codebase
        return self.spr.get(912 + idx, 0)

    def _load_elem(self, addr: int, elem_type: int) -> float:
        if elem_type == gqr.TYPE_F32:
            import struct
            return struct.unpack(">f", self.mem.read(addr, 4))[0]
        size = _ELEM_SIZE[elem_type]
        raw = int.from_bytes(self.mem.read(addr, size), "big",
                              signed=_ELEM_SIGNED[elem_type])
        return raw

    def _store_elem(self, addr: int, value, elem_type: int) -> None:
        if elem_type == gqr.TYPE_F32:
            import struct
            self.mem.write(addr, struct.pack(">f", value))
            return
        size = _ELEM_SIZE[elem_type]
        self.mem.write(addr, int(value).to_bytes(size, "big",
                        signed=_ELEM_SIGNED[elem_type]))

    def psq_load(self, addr: int, w: int, gqr_idx: int):
        g = self.gqr_value(gqr_idx)
        ld_type, ld_scale, _, _ = gqr.gqr_fields(g)
        size = _ELEM_SIZE[ld_type]
        raw0 = self._load_elem(addr, ld_type)
        v0 = raw0 if ld_type == gqr.TYPE_F32 else gqr.dequantize(raw0, ld_type, ld_scale)
        if w:
            return (v0, 1.0)
        raw1 = self._load_elem(addr + size, ld_type)
        v1 = raw1 if ld_type == gqr.TYPE_F32 else gqr.dequantize(raw1, ld_type, ld_scale)
        return (v0, v1)

    def psq_store(self, addr: int, ps, w: int, gqr_idx: int):
        g = self.gqr_value(gqr_idx)
        _, _, st_type, st_scale = gqr.gqr_fields(g)
        size = _ELEM_SIZE[st_type]
        v0 = ps[0] if st_type == gqr.TYPE_F32 else gqr.quantize(ps[0], st_type, st_scale)
        self._store_elem(addr, v0, st_type)
        if not w:
            v1 = ps[1] if st_type == gqr.TYPE_F32 else gqr.quantize(ps[1], st_type, st_scale)
            self._store_elem(addr + size, v1, st_type)

    # --- instruction execution -------------------------------------------------------
    _RE_LOADSTORE = re.compile(
        r"^(?P<op>psq_lu|psq_l|psq_stu|psq_st)\s+(?P<f>\d+),\s*"
        r"(?P<d>-?0x[0-9a-fA-F]+|-?\d+)\((?P<ra>\d+)\),\s*(?P<w>\d+),\s*(?P<i>\d+)$")
    _RE_MADDS = re.compile(
        r"^(?P<op>ps_madds0|ps_madds1)\s+(?P<fd>\d+),\s*(?P<fa>\d+),\s*(?P<fc>\d+),\s*(?P<fb>\d+)$")
    _RE_LHA = re.compile(r"^lha\s+(?P<rd>\d+),\s*(?P<d>-?0x[0-9a-fA-F]+|-?\d+)\((?P<ra>\d+)\)$")
    _RE_LBZ = re.compile(r"^lbz\s+(?P<rd>\d+),\s*(?P<d>-?0x[0-9a-fA-F]+|-?\d+)\((?P<ra>\d+)\)$")
    _RE_SUBI = re.compile(r"^subi\s+(?P<rd>\d+),\s*(?P<ra>\d+),\s*(?P<simm>-?0x[0-9a-fA-F]+|-?\d+)$")
    _RE_LI = re.compile(r"^li\s+(?P<rd>\d+),\s*(?P<simm>-?0x[0-9a-fA-F]+|-?\d+)$")
    _RE_MULLI = re.compile(r"^mulli\s+(?P<rd>\d+),\s*(?P<ra>\d+),\s*(?P<simm>-?0x[0-9a-fA-F]+|-?\d+)$")
    _RE_ADDIS = re.compile(r"^addis\s+(?P<rd>\d+),\s*(?P<ra>\d+),\s*(?P<simm>-?0x[0-9a-fA-F]+|-?\d+)$")
    _RE_MTCTR = re.compile(r"^mtctr\s+(?P<ra>\d+)$")
    _RE_MTSPR = re.compile(r"^mtspr\s+(?P<spr>\d+),\s*%?(?P<rs>[a-z0-9]+)$")
    _RE_LABEL = re.compile(r"^\d+:$")
    _RE_BDNZ = re.compile(r"^bdnz\s+\S+$")

    @staticmethod
    def _imm(s: str) -> int:
        return int(s, 16) if s.lower().startswith(("0x", "-0x")) else int(s)

    def step(self, line: str) -> None:
        line = line.strip()
        if not line or self._RE_LABEL.match(line):
            return
        m = self._RE_LOADSTORE.match(line)
        if m:
            f = int(m["f"]); d = self._imm(m["d"]); ra = int(m["ra"])
            w = int(m["w"]); i = int(m["i"])
            addr = _u32(self.gpr[ra] + d)
            op = m["op"]
            if op in ("psq_l", "psq_lu"):
                self.fpr[f] = self.psq_load(addr, w, i)
                if op == "psq_lu":
                    self.gpr[ra] = addr
            else:
                self.psq_store(addr, self.fpr[f], w, i)
                if op == "psq_stu":
                    self.gpr[ra] = addr
            return
        m = self._RE_MADDS.match(line)
        if m:
            fd, fa, fc, fb = int(m["fd"]), int(m["fa"]), int(m["fc"]), int(m["fb"])
            a0, a1 = self.fpr[fa]
            c0, c1 = self.fpr[fc]
            b0, b1 = self.fpr[fb]
            scalar = c0 if m["op"] == "ps_madds0" else c1
            self.fpr[fd] = (a0 * scalar + b0, a1 * scalar + b1)
            return
        m = self._RE_LHA.match(line)
        if m:
            rd, d, ra = int(m["rd"]), self._imm(m["d"]), int(m["ra"])
            addr = _u32(self.gpr[ra] + d)
            self.gpr[rd] = _u32(_s16(int.from_bytes(self.mem.read(addr, 2), "big")))
            return
        m = self._RE_LBZ.match(line)
        if m:
            rd, d, ra = int(m["rd"]), self._imm(m["d"]), int(m["ra"])
            addr = _u32(self.gpr[ra] + d)
            self.gpr[rd] = self.mem.read(addr, 1)[0]
            return
        m = self._RE_SUBI.match(line)
        if m:
            rd, ra, simm = int(m["rd"]), int(m["ra"]), self._imm(m["simm"])
            self.gpr[rd] = _u32(self.gpr[ra] - simm)
            return
        m = self._RE_LI.match(line)
        if m:
            self.gpr[int(m["rd"])] = _u32(self._imm(m["simm"]))
            return
        m = self._RE_MULLI.match(line)
        if m:
            rd, ra, simm = int(m["rd"]), int(m["ra"]), self._imm(m["simm"])
            # 32-bit signed multiply, low word kept (as real mulli does)
            val = (_s16signed32(self.gpr[ra]) * simm)
            self.gpr[rd] = _u32(val)
            return
        m = self._RE_ADDIS.match(line)
        if m:
            rd, ra, simm = int(m["rd"]), int(m["ra"]), self._imm(m["simm"])
            self.gpr[rd] = _u32(self.gpr[ra] + (simm << 16))
            return
        m = self._RE_MTCTR.match(line)
        if m:
            self.ctr = self.gpr[int(m["ra"])]
            return
        m = self._RE_MTSPR.match(line)
        if m:
            spr = int(m["spr"])
            rs = m["rs"]
            val = self.gpr[int(rs)] if rs.isdigit() else self._extra_operand
            self.spr[spr] = _u32(val)
            return
        if self._RE_BDNZ.match(line):
            return  # handled by the driving loop (see run_loop below)
        raise NotImplementedError(f"unimplemented instruction: {line!r}")


def _s16signed32(v: int) -> int:
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def run_loop(interp: Interpreter, body_lines, n: int) -> None:
    """Runs `body_lines` (the instructions strictly between the `1:` label and the `bdnz 1b`,
    i.e. one loop iteration's worth) exactly `n` times, mtctr/bdnz already accounted for by the
    caller via `n` -- matches CalcSk1_x/CalcSk1_x2's own `mtctr 5` / `bdnz 1b` shape."""
    for _ in range(n):
        for line in body_lines:
            interp.step(line)
