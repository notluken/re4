#!/bin/sh
# Runs test_arena --once N times as fresh processes (default 100) and reports how many succeeded.
# Not a ctest (too slow to run on every build); run by hand or from CI as a periodic check. See
# docs/port-phase2.md, "macOS facts", for why a fresh-process repeat matters here: ASLR and the dyld
# shared cache's own placement can in principle vary between launches even though the arena's own
# request is a fixed address.
set -eu
BIN="${1:-./test_arena}"
N="${2:-100}"
ok=0
fail=0
i=0
while [ "$i" -lt "$N" ]; do
  if "$BIN" --once >/dev/null 2>&1; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    echo "run $i: FAILED"
  fi
  i=$((i + 1))
done
echo "run_arena_stress: $ok/$N ok, $fail failed"
[ "$fail" -eq 0 ]
