#!/bin/bash
# Unit-test the key queues (tty.cpp) on the host. tty.cpp needs only
# screen/uart/wait, all stubbed in tools/ttyhost/stubs.cpp, so it can be
# exercised in milliseconds instead of through a build-and-boot cycle.
set -u
cd "$(dirname "$0")/.."

OUT=${OUT:-/tmp/ttytest_host}
mkdir -p "$(dirname "$OUT")"

g++ -std=c++17 -g -O1 -fno-strict-aliasing \
    -Wall -Wextra -Wno-unused-parameter \
    -o "$OUT" \
    src/kernel/drivers/tty.cpp \
    tools/ttyhost/stubs.cpp \
    tools/ttyhost/runner.cpp || { echo "BUILD FAILED"; exit 1; }

"$OUT"
