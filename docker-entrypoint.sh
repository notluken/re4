#!/usr/bin/env bash
# Bootstraps the native SN GCC (cc1/cc1plus) into the build/ volume before handing off to
# the requested command. build/ is expected to be a *separate* (named) docker volume, not
# the host's build/ (host tools are darwin binaries; sharing it also risks two ninja
# instances -- host and container -- touching the same .ninja_deps/.ninja_log).
set -euo pipefail

# The ProDG link step (ngcld.exe under wibo) opens every input object by two paths (relative and
# its Windows Z:\ absolute form) and doesn't close them promptly; main.elf alone has ~674 inputs.
# Docker's default soft RLIMIT_NOFILE (1024) runs out partway through and wibo reports it as
# `Unhandled errno 24 -> ERROR_NOT_SUPPORTED` (EMFILE), which ngcld then turns into a silent
# non-zero exit. The hard limit is high (raise it here, not via `docker run --ulimit`, so the
# fix travels with the image).
ulimit -n 65536

PRODG_NATIVE_DIR="/re4/build/compilers/ProDG/3.9.3-v1.79"

if [ ! -x "$PRODG_NATIVE_DIR/cc1plus" ] || [ ! -x "$PRODG_NATIVE_DIR/cc1" ]; then
    if [ -n "${SN_GCC_SRC:-}" ]; then
        echo "==> $PRODG_NATIVE_DIR missing cc1/cc1plus; building from SN_GCC_SRC=$SN_GCC_SRC" >&2
        RE4=/re4 /re4/tools/sn-gcc/build.sh
    else
        cat >&2 <<'EOF'
==> WARNING: build/compilers/ProDG/3.9.3-v1.79/{cc1,cc1plus} not found and SN_GCC_SRC is unset.
    `python3 configure.py` (default --prodg-driver native) will exit with a "missing: build it
    with tools/sn-gcc/build.sh" error until you either:
      - mount your own extracted NGC_GNU_SRC/NGC source drop (SN's GPL release) into the
        container and pass -e SN_GCC_SRC=/path/inside/container/NGC, so this entrypoint can
        run tools/sn-gcc/build.sh for you once (cached after that in the build/ volume), or
      - pass --prodg-driver ngccc to configure.py yourself, which uses the downloaded ngccc.exe
        v1.76 pack under wibo instead of the native v1.79 build. This does NOT reproduce ~16
        units byte-for-byte (docs/matching.md, "Compiler") so `dtk shasum` will show more
        failures in that mode.
EOF
    fi
fi

exec "$@"
