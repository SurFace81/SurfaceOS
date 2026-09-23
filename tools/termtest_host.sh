#!/bin/bash
# Unit-test the escape parser on the host. term.cpp only needs four kernel
# functions, all stubbed in tools/termhost/stubs.cpp, so the state machine
# can be exercised in milliseconds instead of through a build-and-boot cycle.
set -u
cd "$(dirname "$0")/.."

OUT=${OUT:-/tmp/termtest_host}
mkdir -p "$(dirname "$OUT")"

g++ -std=c++17 -g -O1 -fno-strict-aliasing \
    -Wall -Wextra -Wno-unused-parameter \
    -o "$OUT" \
    src/kernel/drivers/term.cpp \
    tools/termhost/stubs.cpp \
    tools/termhost/runner.cpp || { echo "BUILD FAILED"; exit 1; }

"$OUT"
