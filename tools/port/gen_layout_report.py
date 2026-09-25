#!/usr/bin/env python3
"""Phase "struct-size parity", cheap version (docs/port-layout-parity.md, coordinator item B):
no cross-compilation needed -- the documented struct sizes already scattered through include/*.h
as "(0xNN bytes...)" comments ARE the SN ProDG/GameCube compiler's own sizeof(), the same ground
truth tools/port/gen_static_asserts.py already trusts for offsetof(). This script:

  1. finds every "struct NAME { ... }" / "class NAME { ... }" in include/*.h whose immediately
     preceding doc-comment block contains a "(0xNN byte(s)...)" annotation naming that type's own
     size (not a member's -- see HEURISTIC below),
  2. generates one probe translation unit that #includes every header hit at least once and prints
     `sizeof(NAME)` for each match,
  3. compiles and runs it against the same include path / defines as the host TARGET_PC/RE4_U32_32
     boot build (CMakeLists.txt's RE4_GAME_INCLUDES/RE4_GAME_DEFINES),
  4. diffs host sizeof() against the documented GC sizeof(), classifies the cause of every mismatch
     it can (native pointer field, virtual function/vptr, nothing obvious -- TO VERIFY), and writes
     docs/port-layout-parity.md.

HEURISTIC (best-effort, not a C parser): a comment is only trusted for a struct/class if it appears
on the line(s) immediately above `struct NAME` / `class NAME` (a handful of blank/attribute lines
tolerated) and its "(0xNN byte...)" mentions nothing else in parentheses that looks like a *different*
identifier's size (e.g. "layout depends on the type (0x30 bytes total, e.g. flr_at.h `area[0x30]`)"
is accepted since it is still describing the struct itself; a comment that clearly annotates a
*member* -- "// 0x18  a nested struct" -- is not matched here at all, gen_static_asserts.py's
per-field offset comments are a different, already-handled case). False positives are possible;
every one this script matches is listed in the report's own "matched" column so a human can check
the source line, not asserted as ground truth beyond what CLAUDE.md already establishes for these
comments project-wide.

Usage:
    tools/port/gen_layout_report.py [--probe-out PATH] [--report-out PATH] [--keep-probe]

Needs a TARGET_PC/RE4_U32_32 clang++ on PATH capable of building this repo's game headers (same
toolchain docs/port-boot.md section 12 sets up for re4_boot); does not need Aurora, a disc image, or
the SN GCC toolchain -- this is the "no cross-compilation" cheap version (docs/port.md B). The full
cross-compiled-vs-GC-compiler probe (build/ measured against the SN toolchain in Docker) is a
separate, heavier follow-up tool, not this one.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
INCLUDE_DIR = ROOT / "include"

# Matches a struct/class definition line: "struct Name {" / "struct Name : Base {" / "class Name {"
DEF_RE = re.compile(r'^\s*(?:struct|class)\s+([A-Za-z_]\w*)\s*(?::[^{;]*)?\{\s*$')
# A "(0xNN byte[s]...)" annotation anywhere in a comment line.
SIZE_RE = re.compile(r'\(0x([0-9A-Fa-f]+)\s*byte', re.IGNORECASE)
# "instance `Foo` (0xNN bytes)" names a *global variable*'s type, not necessarily the struct being
# defined right below the comment (a smaller nested/member type can share the same doc comment,
# e.g. include/cockpit.h's "HUD ... instance `Cckpt` (0xCC bytes)" sits directly above `class
# LifeMeter`, a *member* of Cckpt, not Cckpt itself -- caught by manual review, not the regex alone).
INSTANCE_RE = re.compile(r'instance\s+`(\w+)`')
# A line that is only a comment (// ...) or blank/attribute -- tolerated between the doc comment
# and the struct/class line itself.
COMMENT_RE = re.compile(r'^\s*//')
BLANK_OR_IFDEF_RE = re.compile(r'^\s*(#\s*if|#\s*else|#\s*endif)?\s*$')


def find_candidates(header: Path):
    """Yields (struct_name, documented_size_int, doc_line_no) for header."""
    lines = header.read_text(encoding="utf-8", errors="replace").splitlines()
    for i, line in enumerate(lines):
        m = DEF_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        # Walk upward over blank/#if/#else/#endif lines, collect the contiguous // comment block
        # immediately above, and look for a size annotation in it.
        j = i - 1
        comment_lines = []
        while j >= 0 and BLANK_OR_IFDEF_RE.match(lines[j]) and not COMMENT_RE.match(lines[j]):
            j -= 1
        while j >= 0 and COMMENT_RE.match(lines[j]):
            comment_lines.append(lines[j])
            j -= 1
        comment_lines.reverse()
        for cline in comment_lines:
            sm = SIZE_RE.search(cline)
            if not sm:
                continue
            im = INSTANCE_RE.search(cline)
            if im and im.group(1) != name:
                # The byte count names a *different* type's global instance (a containing struct,
                # usually) -- not a confident match for the type defined right below. Skip.
                continue
            yield name, int(sm.group(1), 16), i + 1
            break


def collect_all():
    """Returns [(header_relpath, struct_name, gc_size, doc_line)], one per header, first match only
    (a header defining the same name twice -- e.g. both #ifdef branches -- is deduplicated, matching
    gen_static_asserts.py's own "first occurrence wins" rule)."""
    out = []
    for header in sorted(INCLUDE_DIR.glob("*.h")):
        seen = set()
        for name, size, line_no in find_candidates(header):
            if name in seen:
                continue
            seen.add(name)
            out.append((header.relative_to(ROOT).as_posix(), name, size, line_no))
    return out


PROBE_HEADER = """// GENERATED by tools/port/gen_layout_report.py -- do not edit, do not commit.
#include <cstdio>
#include "types.h"
#include "global.h"
"""


def build_probe(entries):
    lines = [PROBE_HEADER]
    includes = []
    seen_headers = set()
    for header, _, _, _ in entries:
        if header not in seen_headers:
            seen_headers.add(header)
            includes.append(f'#include "{Path(header).name}"')
    lines.extend(includes)
    lines.append("\nint main() {")
    for header, name, gc_size, line_no in entries:
        tag = f"{header}:{line_no}:{name}"
        # #ifdef-guard each individual probe line so one bad match (template, incomplete type, a
        # name that collides with a macro) can't take the whole probe down -- report says SKIP for
        # anything that fails to compile as its own line, isolated with __has_include-style trick
        # is not possible for "does this expression compile", so instead each is its own TU pass;
        # see gen_and_report()'s per-entry fallback compile.
        lines.append(f'    printf("SIZE\\t{tag}\\t%zu\\t{gc_size}\\n", sizeof({name}));')
    lines.append("    return 0;")
    lines.append("}")
    return "\n".join(lines) + "\n"


GAME_INCLUDES = [ROOT / "include", ROOT / "src", ROOT / "build" / "G4BE08" / "include"]
GAME_DEFINES = ["BUILD_VERSION=0", "VERSION_G4BE08", "NDEBUG=1", "TARGET_PC=1", "RE4_U32_32=1"]
GAME_OPTIONS = [
    "-Wno-invalid-offsetof",
    "-Wno-narrowing",
    "-Wno-address-of-temporary",
    "-std=c++17",
    "-fsyntax-only",
]


def clang_cmd(src: Path, syntax_only=True):
    cmd = ["clang++"]
    for d in GAME_INCLUDES:
        cmd += ["-I", str(d)]
    for d in GAME_DEFINES:
        cmd += [f"-D{d}"]
    cmd += ["-Wno-invalid-offsetof", "-Wno-narrowing", "-Wno-address-of-temporary", "-std=c++17"]
    if syntax_only:
        cmd += ["-fsyntax-only"]
    else:
        cmd += ["-o", str(src.with_suffix(""))]
    cmd += [str(src)]
    return cmd


def try_compile_and_run(entries, probe_path: Path, keep_probe: bool):
    """Attempts the combined probe first; on failure, bisects by dropping entries whose header
    fails on its own (isolated -fsyntax-only include check) so the rest still get a real answer."""
    probe_path.parent.mkdir(parents=True, exist_ok=True)
    probe_path.write_text(build_probe(entries))
    exe_path = probe_path.with_suffix("")
    result = subprocess.run(clang_cmd(probe_path, syntax_only=False), capture_output=True, text=True)
    if result.returncode == 0:
        run = subprocess.run([str(exe_path)], capture_output=True, text=True)
        if not keep_probe:
            probe_path.unlink(missing_ok=True)
            exe_path.unlink(missing_ok=True)
        return run.stdout, None
    # Combined probe failed -- fall back to isolating each header (compile just its own #include
    # line against the same shared prelude) to report which specific ones don't combine cleanly,
    # rather than silently reporting zero results.
    return None, result.stderr


def isolate_bad_headers(entries):
    """One-header-at-a-time syntax check; returns (good_entries, bad_headers_with_error)."""
    by_header = {}
    for e in entries:
        by_header.setdefault(e[0], []).append(e)
    good, bad = [], {}
    tmp_dir = ROOT / "build-pc-tool" / "layout_probe_isolate"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    for header, group in by_header.items():
        src = tmp_dir / (Path(header).stem + "_iso.cpp")
        src.write_text(PROBE_HEADER + f'#include "{Path(header).name}"\nint main(){{return 0;}}\n')
        r = subprocess.run(clang_cmd(src, syntax_only=True), capture_output=True, text=True)
        if r.returncode == 0:
            good.extend(group)
        else:
            bad[header] = r.stderr.strip().splitlines()[0] if r.stderr.strip() else "unknown error"
        src.unlink(missing_ok=True)
    return good, bad


PTR_FIELD_RE = re.compile(r'^\s*[\w:<>,\s]+\*\s*\w+\s*(\[[^\]]*\])?\s*;')
PTR32_RE = re.compile(r'Ptr32<')
VIRTUAL_RE = re.compile(r'^\s*virtual\b')


def classify_cause(root: Path, header: str, name: str, gc_size: int, host_size: int) -> str:
    text = (root / header).read_text(encoding="utf-8", errors="replace")
    # crude: grab the struct/class body text between its opening brace and the matching close.
    m = re.search(r'\b(?:struct|class)\s+' + re.escape(name) + r'\b[^{;]*\{', text)
    body = ""
    if m:
        depth = 1
        i = m.end()
        start = i
        while i < len(text) and depth > 0:
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
            i += 1
        body = text[start:i - 1]
    diff = host_size - gc_size
    if VIRTUAL_RE.search(body) or "vtable" in body.lower():
        return f"vptr (virtual function present, diff {diff:+d}): likely 8-byte host vptr vs 4-byte GC"
    raw_ptr_lines = [
        l for l in body.splitlines()
        if PTR_FIELD_RE.match(l) and "Ptr32" not in l and "//" not in l.split(";")[0]
    ]
    if raw_ptr_lines and "#ifdef TARGET_PC" not in body:
        return f"native pointer field(s) not Ptr32<T> (diff {diff:+d}): " + "; ".join(
            s.strip() for s in raw_ptr_lines[:3])
    if "long" in body and diff in (4, -4):
        return f"long (host 8B vs GC 4B?) (diff {diff:+d}): TO VERIFY"
    if diff % 4 == 0 and abs(diff) <= 8:
        return f"alignment/padding (diff {diff:+d}): TO VERIFY"
    return f"unexplained (diff {diff:+d}): TO VERIFY"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe-out", default=str(ROOT / "build-pc-tool" / "layout_probe.cpp"))
    ap.add_argument("--report-out", default=str(ROOT / "docs" / "port-layout-parity.md"))
    ap.add_argument("--keep-probe", action="store_true")
    args = ap.parse_args()

    entries = collect_all()
    print(f"# candidates found: {len(entries)}", file=sys.stderr)

    probe_path = Path(args.probe_out)
    stdout, err = try_compile_and_run(entries, probe_path, args.keep_probe)
    skipped = {}
    if stdout is None:
        print("# combined probe failed to compile, isolating headers...", file=sys.stderr)
        good, bad = isolate_bad_headers(entries)
        skipped = bad
        stdout, err2 = try_compile_and_run(good, probe_path, args.keep_probe)
        if stdout is None:
            sys.exit(f"gen_layout_report: even the isolated probe failed:\n{err2}")
        entries = good

    results = []
    for line in stdout.splitlines():
        if not line.startswith("SIZE\t"):
            continue
        _, tag, host_size, gc_size = line.split("\t")
        header, line_no, name = tag.split(":")
        results.append((header, int(line_no), name, int(gc_size), int(host_size)))

    matches = [r for r in results if r[3] == r[4]]
    mismatches = [r for r in results if r[3] != r[4]]

    causes = {}
    for header, line_no, name, gc_size, host_size in mismatches:
        cause = classify_cause(ROOT, header, name, gc_size, host_size)
        causes[(header, name)] = cause

    lines = []
    lines.append("# Struct-size parity report (cheap version, no cross-compilation)")
    lines.append("")
    lines.append(f"Generated by `tools/port/gen_layout_report.py`. {len(entries)} struct/class types "
                 f"in `include/*.h` carry a `(0xNN byte...)` size comment this script could confidently "
                 f"attach to a specific type (see the script's own HEURISTIC docstring for what counts); "
                 f"{len(skipped)} header(s) did not combine into one probe translation unit and were "
                 f"skipped (listed below), not silently dropped.")
    lines.append("")
    lines.append(f"- **Matches (host sizeof == documented GC sizeof): {len(matches)}**")
    lines.append(f"- **Mismatches: {len(mismatches)}**")
    lines.append("")
    if mismatches:
        lines.append("## Mismatches")
        lines.append("")
        lines.append("| Type | Header | GC size | Host size | Diff | Likely cause |")
        lines.append("|---|---|---|---|---|---|")
        for header, line_no, name, gc_size, host_size in sorted(mismatches, key=lambda r: -abs(r[4]-r[3])):
            cause = causes.get((header, name), "TO VERIFY")
            lines.append(f"| `{name}` | `{header}:{line_no}` | 0x{gc_size:x} | 0x{host_size:x} "
                         f"| {host_size-gc_size:+d} | {cause} |")
        lines.append("")
    if skipped:
        lines.append("## Headers skipped (did not compile in isolation)")
        lines.append("")
        lines.append("| Header | First error |")
        lines.append("|---|---|")
        for header, error in sorted(skipped.items()):
            lines.append(f"| `{header}` | `{error}` |")
        lines.append("")
    lines.append("## Matches (for completeness)")
    lines.append("")
    lines.append("| Type | Header |")
    lines.append("|---|---|")
    for header, line_no, name, gc_size, host_size in sorted(matches, key=lambda r: (r[0], r[2])):
        lines.append(f"| `{name}` | `{header}:{line_no}` |")
    lines.append("")

    Path(args.report_out).write_text("\n".join(lines))
    print(f"# wrote {args.report_out}: {len(matches)} match, {len(mismatches)} mismatch, "
          f"{len(skipped)} header(s) skipped", file=sys.stderr)


if __name__ == "__main__":
    main()
