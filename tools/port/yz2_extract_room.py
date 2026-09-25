#!/usr/bin/env python3
"""Test-time helper for tests/port/test_yz2.cpp: given a GameCube disc image (.iso/.gcm) and a room
archive path inside it (e.g. "st1/r120.das"), writes two files:
  <out_raw>: the yz2 stream exactly as Yz2DecodeSet/Yz2DecodeExec (src/game/yz2code.cpp) would see
             it -- the ASCII header ("<packed hex> <unpacked hex>") followed by the coded bytes,
             starting wherever tools/motion/yz2.is_yz2() finds that header (offset 0 or, for this
             debug build's room archives, 0x400 past a container header -- see docs/port-phase3.md,
             "room data formats").
  <out_ref>: the decompressed body, decoded by tools/motion/yz2.decode() (the trusted reference).

Never commits disc data: both outputs are written under a build/test scratch directory by the
caller (CMake test fixture), not this repo.

usage: yz2_extract_room.py <disc.iso|disc.gcm> <room/path.das> <out_raw> <out_ref>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'motion'))
import yz2  # noqa: E402


class Gcm:
    """Minimal GameCube disc image reader (FST walk only; no yaml dependency, unlike
    tools/extract_orig.py's copy of this class)."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(0x420)
        self.dol_off, fst_off, fst_size = struct.unpack('>3I', self.f.read(12))
        self.f.seek(fst_off)
        fst = self.f.read(fst_size)
        count = struct.unpack('>I', fst[8:12])[0]
        names = fst[12 * count:]
        self.files = {}
        stack = [('', count)]
        i = 1
        while i < count:
            while i >= stack[-1][1]:
                stack.pop()
            flags_name, a, b = struct.unpack('>3I', fst[12 * i:12 * i + 12])
            name = names[flags_name & 0xFFFFFF:].split(b'\0', 1)[0].decode('shift_jis')
            path = f'{stack[-1][0]}/{name}' if stack[-1][0] else name
            if flags_name >> 24:
                stack.append((path, b))
            else:
                self.files[path.lower()] = (path, a, b)
            i += 1

    def read_all(self, path):
        _, a, b = self.files[path.lower()]
        self.f.seek(a)
        return self.f.read(b)


def main():
    disc, room, out_raw, out_ref = sys.argv[1:5]
    g = Gcm(disc)
    data = g.read_all(room)
    base = 0 if yz2.is_yz2(data) else 0x400
    _, unpacked, start = yz2.parse_header(data, base)
    ref, packed, used = yz2.decode(data, base)
    assert len(ref) == unpacked
    with open(out_raw, 'wb') as f:
        f.write(data[base:start + used])  # header + padding + exactly the coded bytes read
    with open(out_ref, 'wb') as f:
        f.write(ref)
    print(f'{room}: base {base:#x}, packed {packed:#x}, unpacked {len(ref):#x}')


if __name__ == '__main__':
    main()
