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
"""
import importlib.util
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

spec = importlib.util.spec_from_file_location("modules", ROOT / "config" / "G4BE08" / "modules.py")
modules_mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(modules_mod)
UNITS = getattr(modules_mod, "UNITS", {})

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
# once here instead of once per module, so it only gets one of its several real REL_MODULE values;
# fine for an error inventory, not for matching).
for s in sorted(sources):
    print(f"src/{s} {sources[s]}")
