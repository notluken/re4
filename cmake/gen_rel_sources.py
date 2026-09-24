#!/usr/bin/env python3
"""Prints the source files of every REL module (config/G4BE08/config.yml `modules:` + modules.py
UNITS), one repo-relative `src/...` path per line, for CMakeLists.txt's re4_rel_all target.

This mirrors configure.py's own REL object loop (see configure.py, "REL module units:") without
needing the toolchain/dtk/disc images configure.py itself requires, since we only read modules.py +
config.yml as plain data. Never writes anything; read-only helper for the host build.

Module "Tools" is a special case (see the printed comment lines starting with '#'): on a
case-insensitive filesystem (APFS) src/Tools/ and src/tools/ are the same directory, and three
filenames exist in both with different content (Tools/{t_prim,t_util,tools}.cpp are 3-line module
wrappers; tools/{t_prim,t_util,tools}.cpp are the shared bodies used by the t_* tool modules). On
such a host, reading "src/Tools/t_prim.cpp" silently returns whichever of the two the filesystem
folded onto the shared inode -- not necessarily the "Tools" module's own file. This script omits
those three Tools/-module units rather than guess which content is really on disk; every other unit
(including the "tools/" shared copies used by t_camera/t_emlist/...) is unaffected, since it has no
same-named counterpart in the other directory.
"""
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

config_yml = (ROOT / "config" / "G4BE08" / "config.yml").read_text()
module_names = re.findall(r"^\s+name:\s*(\S+)\s*$", config_yml, re.M)

# The three case-fold-corrupted "Tools" module units (see the docstring above).
SKIP_APFS_FOLD = {"Tools/t_prim.cpp", "Tools/t_util.cpp", "Tools/tools.cpp"}

sources = {}  # src path -> first module that owns it (for the REL_MODULE define)
skipped = set()
for mod in module_names:
    for unit, _first, *_src in UNITS.get(mod, [(f"{mod}/{mod}.cpp", None)]):
        src = _src[0] if _src and _src[0] else unit
        if src in SKIP_APFS_FOLD:
            skipped.add(src)
            continue
        sources.setdefault(src, mod)

# One "src/<path> <owning-module>" line per source (space-separated; no source path in this tree
# contains a space), for CMakeLists.txt to set a per-file REL_MODULE=<mod> definition matching
# configure.py's own -DREL_MODULE=<mod> (a shared source, e.g. st/em_wrap.cpp, is only compiled
# once here instead of once per module, so it only gets one of its several real REL_MODULE values;
# fine for an error inventory, not for matching).
for s in sorted(sources):
    print(f"src/{s} {sources[s]}")
for s in sorted(skipped):
    print(f"# skipped (APFS Tools/tools fold, see docstring): src/{s}", file=sys.stderr)
