# Reproduces the ORIGINAL (matching) re4 build (see README.md / CLAUDE.md).
#
# The repo is meant to be bind-mounted at /re4 at `docker run` time, not copied in, and
# build/ must be a separate (named) volume -- see the usage note below the image build.
#
# Pinned base, forced to linux/amd64: the native SN GCC (tools/sn-gcc/build.sh) is built
# with `gcc -m32 -static`, and the SN tools that still run under wibo (CPP.exe, NgcAs.exe,
# ngcld.exe) need real x86 segmentation: under amd64 emulation on Apple Silicon they crash
# (Rosetta: "invalid gdt selector index 4", QEMU user-mode: SIGSEGV). Use an x86_64 host.
#
# Clone the repo on a case-sensitive filesystem: src/Tools/ and src/tools/ are distinct
# directories, and a bind mount of a macOS (APFS, case-insensitive) checkout folds them
# into one, so the Tools module would compile the wrong files.
FROM --platform=linux/amd64 ubuntu:24.04

# Build/runtime dependencies:
#   - build-essential, gcc-multilib, g++-multilib, patch: to build SN's native cc1/cc1plus
#     from the GPL source drop (tools/sn-gcc/build.sh / Makefile: -m32 -static host gcc).
#     gcc-multilib needs the i386 architecture enabled for its 32-bit libc/headers.
#   - python3, python3-yaml: configure.py and every tool under tools/ (stdlib only, except
#     tools/extract_orig.py / tools/gen_rel_config.py which need PyYAML for config.yml).
#   - ninja-build: the actual build driver.
#   - ca-certificates + the ability to speak https: configure.py/ninja download
#     decomp-toolkit, objdiff-cli, gc-wii-binutils, wibo and the MWCC compiler pack over
#     the network on first configure (tools/download_tool.py, stdlib urllib -- no curl
#     binary is invoked by the Python side, but dtk/wibo themselves are plain ELF binaries
#     once downloaded).
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        gcc-multilib \
        g++-multilib \
        patch \
        python3 \
        python3-yaml \
        ninja-build \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /re4

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
# Exact command requested: `python3 configure.py && ninja`. The entrypoint runs before
# this to (re)build the native SN GCC into the build/ volume when it's missing.
CMD ["bash", "-lc", "python3 configure.py && ninja"]

# --- Usage ---
#
# docker build --platform linux/amd64 -t re4-build .
#
# One-time compiler build + verify build, with your own extracted SN GPL source drop and
# disc image(s) already in orig/G4BE08/ on the host:
#
#   docker run --rm --platform linux/amd64 \
#     -v "$PWD":/re4 -w /re4 \
#     -v re4-build:/re4/build \
#     -v /path/to/NGC_GNU_SRC:/opt/sn-gcc-src:ro \
#     -e SN_GCC_SRC=/opt/sn-gcc-src/NGC \
#     re4-build
#
# Subsequent runs (cc1/cc1plus already cached in the re4-build volume) can drop
# -v .../NGC_GNU_SRC and -e SN_GCC_SRC:
#
#   docker run --rm --platform linux/amd64 \
#     -v "$PWD":/re4 -w /re4 \
#     -v re4-build:/re4/build \
#     re4-build
#
# Verify (three checks from CLAUDE.md), same mounts, overriding CMD:
#
#   docker run --rm --platform linux/amd64 \
#     -v "$PWD":/re4 -w /re4 -v re4-build:/re4/build \
#     re4-build bash -lc '
#       find build/G4BE08 -path "*/obj/*" -prune -o -name "*.o" -print | xargs rm -f &&
#       ninja &&
#       build/tools/dtk shasum -c config/G4BE08/build.sha1'
