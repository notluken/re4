#!/usr/bin/env python3
"""Prints the source files of every REL module (config/G4BE08/config.yml `modules:` + modules.py
UNITS), one repo-relative `src/...` path per line, for CMakeLists.txt's re4_rel_all target.

This mirrors configure.py's own REL object loop (see configure.py, "REL module units:") without
needing the toolchain/dtk/disc images configure.py itself requires, since we only read modules.py +
config.yml as plain data. Never writes anything; read-only helper for the host build.

Every source path modules.py names is a real, distinct file: the "Tools" module's own units (unit
names keep the vendor-facing "Tools/..." spelling; their physical source lives in src/tools_mod/,
distinct from the shared src/tools/ bodies the other tool modules use -- see docs/port.md, "the
Tools/tools APFS fold" and its rename fix) no longer share a directory with src/tools/ on a
case-insensitive filesystem, so nothing is skipped here.

--per-module <mod>: for one real REL module (the actual-linking mode, docs/port.md's REL plan step
2, as opposed to the error-inventory mode above), print every one of ITS OWN units -- including
ones a shared source like st/em_wrap.cpp or wep/pl_handgun.cpp -- without collapsing them against
any other module's copy of the same source (each module compiles its own object from that source,
with its own CFLAGS/UNIT_CFLAGS and its own -DREL_MODULE; configure.py's real REL build already
does this once per module, the same thing `sources.setdefault()` above deliberately does NOT do,
since that dict exists only to pick one owner for the inventory build's single shared copy). One
tab-separated line per unit: `<src-relpath>\t<space-separated extra cflags, possibly empty>`.
"""
import argparse
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

spec = importlib.util.spec_from_file_location("modules", ROOT / "config" / "G4BE08" / "modules.py")
modules_mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(modules_mod)
UNITS = getattr(modules_mod, "UNITS", {})
CFLAGS = getattr(modules_mod, "CFLAGS", {})
UNIT_CFLAGS = getattr(modules_mod, "UNIT_CFLAGS", {})


def per_module(mod):
    units = UNITS.get(mod, [(f"{mod}/{mod}.cpp", None)])
    mod_flags = CFLAGS.get(mod, [])
    for entry in units:
        unit, _first, *_src = entry
        src = _src[0] if _src and _src[0] else unit
        # UNIT_CFLAGS keys are the vendor-facing unit name (e.g. "Tools/db_toolbase.cpp"), appended
        # after the module's own CFLAGS (modules.py's own comment: "a later -f flag wins").
        flags = mod_flags + UNIT_CFLAGS.get(unit, [])
        print(f"src/{src}\t{' '.join(flags)}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-module", metavar="MOD")
    args = ap.parse_args()

    if args.per_module:
        if args.per_module not in UNITS and args.per_module not in (
            re.findall(r"^\s+name:\s*(\S+)\s*$", (ROOT / "config" / "G4BE08" / "config.yml").read_text(), re.M)
        ):
            print(f"gen_rel_sources.py: unknown module {args.per_module!r}", file=sys.stderr)
            sys.exit(1)
        per_module(args.per_module)
        sys.exit(0)

    config_yml = (ROOT / "config" / "G4BE08" / "config.yml").read_text()
    module_names = re.findall(r"^\s+name:\s*(\S+)\s*$", config_yml, re.M)

    sources = {}  # src path -> first module that owns it (for the REL_MODULE define)
    for mod in module_names:
        for unit, _first, *_src in UNITS.get(mod, [(f"{mod}/{mod}.cpp", None)]):
            src = _src[0] if _src and _src[0] else unit
            sources.setdefault(src, mod)

    # One "src/<path> <owning-module>" line per source (space-separated; no source path in this tree
    # contains a space), for CMakeLists.txt to set a per-file REL_MODULE=<mod> definition matching
    # configure.py's own -DREL_MODULE=<mod> (a shared source, e.g. st/em_wrap.cpp, is only compiled
    # once here instead of once per module, so it only gets one of its several real REL_MODULE
    # values; fine for an error inventory, not for matching).
    for s in sorted(sources):
        print(f"src/{s} {sources[s]}")
