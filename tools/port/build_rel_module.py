#!/usr/bin/env python3
"""REL plan steps 2-4 (docs/port-boot.md), driver for ONE statically-linked REL module.

Wraps the pipeline CMakeLists.txt would otherwise have to express as several OBJECT libraries plus
two raw `ld -r` invocations (verified by hand first -- every step below is exactly what was run
manually against `st1_0` and checked with `nm`/`otool` before this script existed) into one
add_custom_command per module, so `re4_boot`'s own CMakeLists.txt block only needs to name the
module and consume its one output object.

Steps:
  1. tools/port/gen_rel_module.py <mod> --out-dir <gen-dir>: this module's own section.h (the
     per-module `#pragma clang section data/bss/rodata`) and desc.cpp (its `RelModuleDesc`).
  2. cmake/gen_rel_sources.py --per-module <mod>: this module's own unit list, WITHOUT collapsing
     shared sources against any other module's copy (unlike the error-inventory `re4_rel_all`'s
     default mode) -- each of THIS module's own objects, with THIS module's own CFLAGS/UNIT_CFLAGS.
  3. Compile every unit (--cast-rewriter run first if a pointer<->smaller-int cast makes the plain
     compile fail -- RE4_U32_32=ON needs it the same way re4_game_all's own RE4_REWRITE_CASTS does;
     tried plain first per file, only re-tried through the rewriter on a real cast-narrowing error,
     since most REL units, unlike most src/game/ units, do NOT need it) with RE4_U32_32,
     -fno-zero-initialized-in-bss (a REL's .data/.bss boundary is a real, separately-addressed
     split the loader computes from the header's bssSize field -- letting clang's default
     zero-initialized-global-goes-to-bss optimization move an explicitly-initialized `= 0` vendor
     global out of .data the way it would for an ordinary DOL global would silently change which
     GameCube section that global's address falls in), and this module's own section.h.
  4. Two-phase `ld -r`: phase 1 combines every compiled object (units + desc.cpp) into one
     relocatable object; phase 2 re-links THAT through -exported_symbols_list, exporting ONLY the
     module's own `<Mod>_GetModuleDesc` accessor. Every vendor-named symbol the module defines
     (`_prolog` above all: EVERY stage/room module's own st1.cpp/st2.cpp/st4.cpp copy defines one)
     would otherwise collide with every other module's identically-named copy once two modules'
     objects are linked into the very same executable -- ld -r's own visibility restriction (not a
     rename) hides them (Mach-O "private extern"/local, confirmed with `nm -m`) instead, which is
     exactly what a real GameCube REL's own per-module symbol namespace already gave it for free
     (each REL is its own separately-loaded relocatable file there; this port links every module
     into ONE host executable instead, so it has to recreate that isolation explicitly).

Output: <out-dir>/<mod>_final.o -- feed this straight into re4_boot's link line (a plain object
file among CMake sources is passed straight to the linker, not recompiled).
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def sh(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        print(f"build_rel_module.py: command failed ({r.returncode}): {' '.join(map(str, cmd))}",
              file=sys.stderr)
        sys.exit(1)


def module_ident(mod):
    return mod[0].upper() + mod[1:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module")
    ap.add_argument("--out-dir", required=True, help="both the generated section.h/desc.cpp AND the built objects land here")
    ap.add_argument("--cast-rewriter", required=True)
    ap.add_argument("--resource-dir", required=True)
    ap.add_argument("--sysroot", required=True)
    ap.add_argument("--compile-db-dir", required=True, help="a build dir with a compile_commands.json the cast rewriter can use")
    ap.add_argument("--includes", nargs="*", default=[])
    ap.add_argument("--defines", nargs="*", default=[])
    ap.add_argument("--cxx", default="c++")
    args = ap.parse_args()

    mod = args.module
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Step 1: section.h / desc.cpp
    sh([sys.executable, str(ROOT / "tools/port/gen_rel_module.py"), mod, "--out-dir", str(out_dir)])
    section_h = out_dir / "section.h"

    # Step 2: this module's own unit list (tab-separated: src-relpath, extra cflags)
    out = subprocess.run([sys.executable, str(ROOT / "cmake/gen_rel_sources.py"), "--per-module", mod],
                          cwd=ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr, file=sys.stderr)
        sys.exit(1)

    common_flags = [
        "-DBUILD_VERSION=0", "-DNDEBUG=1", "-DRE4_U32_32", "-DTARGET_PC", "-DVERSION_G4BE08",
        f"-DREL_MODULE={mod}",
        "-std=gnu++17", "-arch", "arm64", "-g",
        "-Wno-invalid-offsetof", "-Wno-narrowing", "-Wno-address-of-temporary",
        "-mllvm", "-global-isel=false",
        "-fno-zero-initialized-in-bss",
    ] + [f"-I{i}" for i in args.includes] + [f"-D{d}" for d in args.defines] + [
        f"-I{out_dir}",
        "-include", str(section_h),
        "-include", str(ROOT / "include/port/ptr32.h"),
        "-include", str(ROOT / "include/port/layout_asserts.h"),
    ]

    objs = []
    for line in out.stdout.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        relpath = parts[0]
        extra = parts[1].split() if len(parts) > 1 and parts[1] else []
        src = ROOT / relpath
        name = Path(relpath).stem
        obj = out_dir / f"{name}.o"

        # Try the plain source first (most REL units need no rewriting at all -- measured on
        # st1_0: 2 of 4 units); only fall back to the cast rewriter on the exact diagnostic it
        # exists to fix, so a real, unrelated compile error is not silently masked by rewriting.
        r = subprocess.run([args.cxx] + common_flags + extra + ["-c", str(src), "-o", str(obj)],
                            capture_output=True, text=True)
        if r.returncode != 0 and "cast from pointer to smaller type" in r.stderr:
            rewritten = out_dir / f"{name}.rewritten.cpp"
            sh([args.cast_rewriter, "-p", args.compile_db_dir, str(src), "-o", str(rewritten),
                "--report", str(out_dir / f"{name}.report.txt"),
                f"--resource-dir={args.resource_dir}",
                "--extra-arg=-isysroot", f"--extra-arg={args.sysroot}",
                "--extra-arg=-URE4_U32_32", "--extra-arg=-fms-extensions",
                "--extra-arg=-Wno-error", "--extra-arg=-ferror-limit=0"])
            sh([args.cxx] + common_flags + extra + ["-c", str(rewritten), "-o", str(obj)])
        elif r.returncode != 0:
            print(r.stderr, file=sys.stderr)
            sys.exit(1)
        objs.append(str(obj))

    # desc.cpp itself: part of the SAME combine (its _prolog/_epilog/_unresolved references must
    # resolve to this module's own local copies before phase 2 hides them).
    desc_obj = out_dir / "desc.o"
    sh([args.cxx] + common_flags + ["-c", str(out_dir / "desc.cpp"), "-o", str(desc_obj)])
    objs.append(str(desc_obj))

    # Step 4a: combine first, THEN find this module's own cross-module room-function imports (the
    # setTbl()-registered R<hex>Init/R<hex>Main pairs a shared object like st1.cpp calls for every
    # room in the whole "links" group, docs/port-boot.md's REL plan step (d)) that stayed undefined
    # -- this module's own units define SOME of them (r100.cpp/r120.cpp here), the rest belong to
    # st1_1/st1_2/st1_3, not built yet. A REAL GameCube resolves those at OSLink time from whichever
    # sibling modules happen to be loaded alongside this one; nothing plays that role yet (explicitly
    # deferred -- OSLink stays a stub, next slice), so give each one a WEAK definition that calls
    # this module's own `_unresolved()` -- exactly what the real loader binds an unresolved import
    # to (OSModuleHeader.unresolved, include/dolphin/os/OSModule.h) -- so the module links and, if
    # ever actually reached before its sibling module exists, HALTs loudly instead of jumping into
    # garbage. `weak` (not a hard definition) on purpose: a later slice building st1_1 too will link
    # st1_1's own STRONG `R101Init`/... right over this one (standard weak-symbol override), the
    # same coalescing OSLink's real relocation would have done, with no further change needed here.
    combined_pre = out_dir / f"{mod}_combined_pre.o"
    sh(["ld", "-r", "-arch", "arm64"] + objs + ["-o", str(combined_pre)])

    undef = subprocess.run(["nm", "-m", str(combined_pre)], capture_output=True, text=True).stdout
    room_syms = set()
    for line in undef.splitlines():
        if "undefined" not in line:
            continue
        m = re.search(r"\b__Z\d+(R[0-9a-fA-F]+(?:Init|Main))v\b", line)
        if m:
            room_syms.add(m.group(1))

    if room_syms:
        stub_src = out_dir / "weak_room_stubs.cpp"
        lines = [
            "// GENERATED by tools/port/build_rel_module.py -- weak fallbacks for this \"links\""
            f" group's room Init/Main functions {mod} itself does not define (see this script's own"
            " comment). Regenerate, do not hand-edit.",
            'extern "C" void _unresolved();',
        ]
        for name in sorted(room_syms):
            lines.append(f'__attribute__((weak)) void {name}() {{ _unresolved(); }}')
        stub_src.write_text("\n".join(lines) + "\n")
        stub_obj = out_dir / "weak_room_stubs.o"
        sh([args.cxx] + common_flags + ["-c", str(stub_src), "-o", str(stub_obj)])
        objs.append(str(stub_obj))

    # Step 4b: re-combine with the weak stubs included, then restrict visibility.
    combined = out_dir / f"{mod}_combined.o"
    sh(["ld", "-r", "-arch", "arm64"] + objs + ["-o", str(combined)])

    exports = out_dir / "exports.txt"
    exports.write_text(f"_{module_ident(mod)}_GetModuleDesc\n")

    final = out_dir / f"{mod}_final.o"
    sh(["ld", "-r", "-arch", "arm64", str(combined), "-o", str(final),
        "-exported_symbols_list", str(exports)])

    print(str(final))


if __name__ == "__main__":
    main()
