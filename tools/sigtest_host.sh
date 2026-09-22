#!/bin/bash
# Unit-test the signal core on the host. src/kernel/cpu/signal.cpp is
# deliberately free of kernel dependencies - it is arithmetic over a mask
# and a saved CPU context - so it needs no stubs at all, and the rules that
# are easy to get quietly wrong (what SIGKILL ignores, what execve resets,
# where the sigframe lands) can be checked in milliseconds.
set -u
cd "$(dirname "$0")/.."

OUT=${OUT:-/tmp/sigtest_host}
mkdir -p "$(dirname "$OUT")"

g++ -std=c++17 -g -O1 -fno-strict-aliasing \
    -Wall -Wextra -Wno-unused-parameter \
    -o "$OUT" \
    src/kernel/cpu/signal.cpp \
    tools/sighost/runner.cpp || { echo "BUILD FAILED"; exit 1; }

"$OUT"
