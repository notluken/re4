#!/usr/bin/env python3
"""Stub generator v2 for re4_boot's undefined-symbol list (docs/port.md "First boot", docs/port-boot.md).

Input: a linker undefined-symbol list, one per line, exactly as macOS `ld` prints it in the
"Undefined symbols for architecture arm64" block -- i.e. the quoted name with the surrounding
quotes stripped (see tools/port/collect_undef.sh, or just `grep '^  "' link.log | sed ...`).
Handles two shapes:
  - C-linkage names (`_Foo`, one leading Mach-O underscore, no `::`): looked up as an `extern "C"`
    declaration in include/*.h. A function gets a logging-once stub returning a default value; a
    data symbol gets a zero-initialized definition of the declared type.
  - Already-demangled C++ names (`Class::method(ArgT, ArgT2)`, `Class::Class()`, or a bare
    `vtable for Class`): the undefined-symbol list itself carries the full signature (macOS ld
    demangles Itanium names in its error text), so no header lookup is needed for the signature --
    only the return type, which is looked up from the class's declaration in include/*.h. Emits an
    out-of-class member-function definition. `vtable for X` entries are NOT stubbed here (the fix is
    a full virtual-function stub set, non-mechanical); they are only listed in the report.

This is a heuristic, best-effort generator, same spirit as the v1 tool it replaces: every generated
declaration should be spot-checked against the header it was matched from. Run:

    python3 tools/port/gen_boot_stubs.py /tmp/undef.txt \
        --out-c src/port/stubs/generated_c_stubs.cpp \
        --out-cpp src/port/stubs/generated_cpp_stubs.cpp \
        --report /tmp/stub_report.txt
"""
import argparse
import re
import subprocess
import sys

ROOT = "/Users/luken/Projects/re4"
DECL_DIRS = ["include", "src/game"]

DEFAULT_RETURNS = {
    "void": "",
    "int": "return 0;",
    "u32": "return 0;",
    "s32": "return 0;",
    "u16": "return 0;",
    "s16": "return 0;",
    "u8": "return 0;",
    "s8": "return 0;",
    "f32": "return 0.0f;",
    "float": "return 0.0f;",
    "double": "return 0.0;",
    "BOOL": "return 0;",
    "bool": "return false;",
    "long": "return 0;",
    "unsigned int": "return 0;",
    "unsigned long": "return 0;",
}


def grep_decls(name):
    try:
        out = subprocess.check_output(
            ["grep", "-rn", rf"\b{re.escape(name)}\b", "--include=*.h", "--include=*.cpp"] + DECL_DIRS,
            cwd=ROOT, text=True)
    except subprocess.CalledProcessError:
        return []
    lines = []
    for line in out.splitlines():
        parts = line.split(":", 2)
        if len(parts) < 3:
            continue
        content = parts[2].strip()
        if content.startswith("//") or content.startswith("*"):
            continue
        # Strip a trailing `// ...` line comment (common on these `extern` declarations) so the
        # `;\s*$` anchors in FUNC_RE/DATA_RE still match -- but don't touch `//` inside a string
        # literal (none of these decls have one, so a plain split is safe here).
        cidx = content.find("//")
        if cidx != -1:
            content = content[:cidx].rstrip()
        lines.append(content)
    return lines


# extern "C" { ... type Name(args); ... } block member, or a lone `extern "C" type Name(args);`
FUNC_RE = re.compile(
    r"^(?:extern\s+\"C\"\s+)?([A-Za-z_][\w:<>\*&\s]*?[\*&]?)\s+(\w+)\s*\(([^;{)]*)\)\s*;\s*$")
# `extern type Name;` or `extern type Name[N];`
DATA_RE = re.compile(
    r"^extern\s+([A-Za-z_][\w:<>]*(?:\s*[\*&])?)\s+(\w+)\s*(\[[^\]]*\])?\s*;\s*$")


def classify_c(name, decls):
    for d in decls:
        d = d.strip()
        m = DATA_RE.match(d)
        if m and m.group(2) == name:
            return ("data", m.group(1).strip(), m.group(3) or "")
        m = FUNC_RE.match(d)
        if m and m.group(2) == name:
            return ("func", m.group(1).strip(), m.group(3).strip())
    return (None, None, None)


CPP_METHOD_RE = re.compile(r"^([\w:<>]+)::(~?\w+|operator\W+)\((.*)\)(\s*const)?$")
CPP_STATIC_MEMBER_RE = re.compile(r"^([\w:<>]+)::(\w+)$")  # e.g. cPlayer::SPEED_RUN_TURN
VTABLE_RE = re.compile(r"^vtable for (\w+)$")


def find_method_return_type(cls, method, argc):
    """Grep the class body in include/*.h for `<ret> method(...)` and return the ret string, or
    None. Ignores overload-arg-count precision (rare in this symbol set) -- first textual match."""
    # naive: search the whole header set for a line mentioning `<ret> method(` inside any file that
    # also defines `class cls`/`struct cls` -- good enough for this ad hoc, single-shot pass.
    try:
        grep_out = subprocess.check_output(
            ["grep", "-rl", rf"class {cls}\b\|struct {cls}\b", "--include=*.h", "include"],
            cwd=ROOT, text=True)
    except subprocess.CalledProcessError:
        return None
    for path in grep_out.splitlines():
        try:
            text = open(f"{ROOT}/{path}").read()
        except OSError:
            continue
        for line in text.splitlines():
            m = re.search(rf"^\s*([A-Za-z_][\w:<>\*&\s]*?)\s+{re.escape(method)}\s*\(", line)
            if m and m.group(1).strip():
                return m.group(1).strip()
    return None


def name_param(ty, idx):
    """Insert `aN` into a parameter type string at the right spot -- most types just take a
    trailing ` aN`, but a function-pointer/array-of-pointer type (`float (*) [4]`) needs the name
    inside the parens (`float (*aN) [4]`)."""
    ty = ty.strip()
    if "(*)" in ty:
        return ty.replace("(*)", f"(*a{idx})", 1)
    return f"{ty} a{idx}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("undef_file")
    ap.add_argument("--out-c", required=True)
    ap.add_argument("--out-cpp", required=True)
    ap.add_argument("--report", required=True)
    args = ap.parse_args()

    lines = [l.rstrip("\n") for l in open(args.undef_file) if l.strip()]

    c_out = ['// AUTO-GENERATED by tools/port/gen_boot_stubs.py -- spot-check before trusting.',
             '#ifdef TARGET_PC', '#include "stub_common.h"', '',
             'extern "C" {']
    # Data symbols at global scope are NOT `extern "C"` in this codebase's own headers (see
    # docs/port-boot.md's mangling note: a non-namespaced global variable isn't Itanium-mangled
    # either way, but `extern "C" T x;` vs `T x;` is still a hard language-linkage mismatch if the
    # two declarations disagree) -- collected separately and emitted after the `extern "C" {...}`
    # block closes, matching each real header declaration's own (non-"C") linkage.
    c_data_out = []
    cpp_out = ['// AUTO-GENERATED by tools/port/gen_boot_stubs.py -- spot-check before trusting.',
               '#ifdef TARGET_PC',
               '#include "stub_common.h"', '']

    report = {"c_func": [], "c_data": [], "c_unresolved": [], "cpp_method": [],
              "cpp_unresolved": [], "vtable": [], "skipped_aurora": []}

    seen_c = set()
    for raw in lines:
        name = raw.strip()
        if not name:
            continue
        vt = VTABLE_RE.match(name)
        if vt:
            report["vtable"].append(vt.group(1))
            continue
        if name.startswith("_") and re.match(r"^_\w+$", name):
            cname = name[1:]
            if cname in seen_c:
                continue
            seen_c.add(cname)
            decls = grep_decls(cname)
            kind, ty, rest = classify_c(cname, decls)
            if kind == "func":
                ret = ty
                body = DEFAULT_RETURNS.get(ret, f"return ({ret}) 0;" if ret not in ("", "void") else "")
                c_out.append(f"// {cname}: `{ty} {cname}({rest})`")
                c_out.append(f"{ret} {cname}({rest})")
                c_out.append("{")
                c_out.append("    static bool warned = false;")
                c_out.append(f'    if (!warned) {{ std::fprintf(stderr, "STUB: {cname}() called\\n"); warned = true; }}')
                if body:
                    c_out.append(f"    {body}")
                c_out.append("}")
                report["c_func"].append(cname)
            elif kind == "data":
                c_data_out.append(f"// {cname}: `extern {ty} {cname}{rest};`")
                c_data_out.append(f"{ty} {cname}{rest}{{}};" if not rest else f"{ty} {cname}{rest} = {{}};")
                report["c_data"].append(cname)
            else:
                report["c_unresolved"].append(cname)
                c_out.append(f"// UNRESOLVED: {cname}" + (f" -- candidates: {decls[:2]}" if decls else " -- no decl found"))
            continue
        # already-demangled C++ name
        m = CPP_METHOD_RE.match(name)
        if m:
            cls, method, argstr, is_const = m.groups()
            args_list = [a.strip() for a in argstr.split(",")] if argstr.strip() else []
            argc = len(args_list)
            params = ", ".join(name_param(t, i) for i, t in enumerate(args_list))
            const_kw = " const" if is_const else ""
            if method == cls or method == f"~{cls}":
                # constructor/destructor: no return type
                cpp_out.append(f"// {name}")
                cpp_out.append(f"{cls}::{method}({params}){const_kw} {{ }}")
                report["cpp_method"].append(name)
            else:
                ret = find_method_return_type(cls, method, argc) or "void"
                body = DEFAULT_RETURNS.get(ret, f"return ({ret}) 0;" if ret not in ("", "void") else "")
                cpp_out.append(f"// {name} -- return type guessed from header: `{ret}`")
                cpp_out.append(f"{ret} {cls}::{method}({params}){const_kw}")
                cpp_out.append("{")
                cpp_out.append(f'    static bool warned = false;')
                cpp_out.append(f'    if (!warned) {{ std::fprintf(stderr, "STUB: {cls}::{method}() called\\n"); warned = true; }}')
                if body:
                    cpp_out.append(f"    {body}")
                cpp_out.append("}")
                report["cpp_method"].append(name)
            continue
        sm = CPP_STATIC_MEMBER_RE.match(name)
        if sm and "(" not in name:
            report["cpp_unresolved"].append(name + " (static/member data -- needs manual definition)")
            continue
        # Free C++ function (global namespace, not extern "C", no class qualifier): `name(args)`.
        fm = re.match(r"^(\w+)\((.*)\)$", name)
        if fm:
            fname, argstr = fm.groups()
            args_list = [a.strip() for a in argstr.split(",")] if argstr.strip() else []
            params = ", ".join(name_param(t, i) for i, t in enumerate(args_list))
            ret = None
            decls = grep_decls(fname)
            for d in decls:
                dm = re.match(rf"^([A-Za-z_][\w:<>\*&\s]*?)\s+{re.escape(fname)}\s*\(", d)
                if dm:
                    ret = dm.group(1).strip()
                    break
            ret = ret or "void"
            body = DEFAULT_RETURNS.get(ret, f"return ({ret}) 0;" if ret not in ("", "void") else "")
            cpp_out.append(f"// {name} -- free function, return type guessed: `{ret}`")
            cpp_out.append(f"{ret} {fname}({params})")
            cpp_out.append("{")
            cpp_out.append(f'    static bool warned = false;')
            cpp_out.append(f'    if (!warned) {{ std::fprintf(stderr, "STUB: {fname}() called\\n"); warned = true; }}')
            if body:
                cpp_out.append(f"    {body}")
            cpp_out.append("}")
            report["cpp_method"].append(name)
            continue
        report["cpp_unresolved"].append(name)

    c_out.append('} // extern "C"')
    c_out.append("")
    c_out.append('// Data symbols (plain C++ linkage -- see the comment above `c_data_out` in this script).')
    c_out.extend(c_data_out)
    c_out.append("#endif // TARGET_PC")
    cpp_out.append("#endif // TARGET_PC")

    with open(f"{ROOT}/{args.out_c}", "w") as f:
        f.write("\n".join(c_out) + "\n")
    with open(f"{ROOT}/{args.out_cpp}", "w") as f:
        f.write("\n".join(cpp_out) + "\n")
    with open(args.report, "w") as f:
        for k, v in report.items():
            f.write(f"== {k} ({len(v)}) ==\n")
            for x in v:
                f.write(f"  {x}\n")
            f.write("\n")

    print(f"c_func={len(report['c_func'])} c_data={len(report['c_data'])} "
          f"c_unresolved={len(report['c_unresolved'])} cpp_method={len(report['cpp_method'])} "
          f"cpp_unresolved={len(report['cpp_unresolved'])} vtable={len(report['vtable'])}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
