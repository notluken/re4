#!/usr/bin/env python3
"""Driver for the Phase 2 cast rewriter (docs/port-phase2.md section 8).

Runs tools/port/cast_rewriter/re4_cast_rewriter once per source file already listed in
build-pc/compile_commands.json (CMake's re4_game_all / re4_rel_all targets, CMAKE_EXPORT_COMPILE_
COMMANDS from the top-level CMakeLists.txt), writing rewritten copies to build-pc/gen/<same
relative path>. Deliberately run against the *default* (RE4_U32_32 OFF) compile command for each
file: with RE4_U32_32 ON, a narrowing pointer<->integer C-style cast is a hard Sema error and clang
does not synthesize a CK_PointerToIntegral/CK_IntegralToPointer AST node for it at all (confirmed
empirically -- see docs/port-phase2.md section 8), so the rewriter would silently see nothing to
rewrite at exactly the sites it exists for. With RE4_U32_32 OFF, u32/s32 are the host's 8-byte
`unsigned long`/`long`, the cast type-checks, and the AST node the rewriter needs to match exists.

Usage:
    python3 tools/port/rewrite_casts.py [--sources-filter SUBSTRING] [--limit N]
        [--compile-commands build-pc/compile_commands.json]
        [--rewriter build-pc-tool/re4_cast_rewriter]
        [--out-dir build-pc/gen]
"""
import argparse
import json
import os
import subprocess
import sys

RESOURCE_DIR_CANDIDATES = [
    "/opt/homebrew/opt/llvm/lib/clang",
]


def find_resource_dir():
    for base in RESOURCE_DIR_CANDIDATES:
        if os.path.isdir(base):
            versions = sorted(os.listdir(base))
            if versions:
                return os.path.join(base, versions[-1])
    # Fall back to asking the homebrew clang directly.
    try:
        out = subprocess.check_output(["/opt/homebrew/opt/llvm/bin/clang", "-print-resource-dir"])
        return out.decode().strip()
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compile-commands", default="build-pc/compile_commands.json")
    ap.add_argument("--rewriter", default="build-pc-tool/re4_cast_rewriter")
    ap.add_argument("--out-dir", default="build-pc/gen")
    ap.add_argument("--report", default="build-pc/gen/cast_rewrite_report.txt")
    ap.add_argument("--sources-filter", default=None,
                     help="only rewrite sources whose path contains this substring")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--sysroot", default=None)
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ccpath = os.path.join(root, args.compile_commands) if not os.path.isabs(args.compile_commands) else args.compile_commands
    if not os.path.exists(ccpath):
        print(f"error: {ccpath} not found -- configure build-pc first (cmake -S . -B build-pc -G Ninja)", file=sys.stderr)
        return 1

    with open(ccpath) as f:
        entries = json.load(f)

    sysroot = args.sysroot
    if sysroot is None:
        try:
            sysroot = subprocess.check_output(["xcrun", "--show-sdk-path"]).decode().strip()
        except Exception:
            sysroot = None

    resource_dir = find_resource_dir()
    if resource_dir is None:
        print("error: could not find a Homebrew LLVM resource dir (brew install llvm?)", file=sys.stderr)
        return 1

    rewriter = os.path.join(root, args.rewriter) if not os.path.isabs(args.rewriter) else args.rewriter
    if not os.path.exists(rewriter):
        print(f"error: {rewriter} not built -- see tools/port/cast_rewriter/CMakeLists.txt", file=sys.stderr)
        return 1

    out_dir = os.path.join(root, args.out_dir) if not os.path.isabs(args.out_dir) else args.out_dir
    report = os.path.join(root, args.report) if not os.path.isabs(args.report) else args.report
    os.makedirs(os.path.dirname(report), exist_ok=True)
    if os.path.exists(report):
        os.remove(report)

    seen = set()
    n = 0
    n_ok = 0
    n_err = 0
    for e in entries:
        src = e["file"]
        if src in seen:
            continue  # a shared REL source appears once per module in the real build; rewrite once
        seen.add(src)
        if args.sources_filter and args.sources_filter not in src:
            continue
        if not src.startswith(root + "/src/"):
            continue
        rel = os.path.relpath(src, root)
        out_path = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(out_path), exist_ok=True)

        cmd_db_dir = os.path.dirname(ccpath)
        cmd = [
            rewriter, "-p", cmd_db_dir, src,
            "-o", out_path,
            "--report", report,
            f"--resource-dir={resource_dir}",
            "--extra-arg=-URE4_U32_32",
            "--extra-arg=-fms-extensions",
            "--extra-arg=-Wno-error",
            "--extra-arg=-ferror-limit=0",
        ]
        if sysroot:
            cmd += ["--extra-arg=-isysroot", f"--extra-arg={sysroot}"]

        n += 1
        proc = subprocess.run(cmd, capture_output=True, text=True)
        ok = os.path.exists(out_path)
        if ok:
            n_ok += 1
        else:
            n_err += 1
            print(f"FAILED (no output): {rel}", file=sys.stderr)
        if args.limit and n >= args.limit:
            break

    print(f"rewrote {n_ok}/{n} source files ({n_err} produced no output) -> {out_dir}")
    print(f"per-site log: {report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
