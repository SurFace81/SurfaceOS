#!/bin/bash
# Unit-test kernel objects and handle tables on the host.
# src/kernel/obj/object.cpp depends on nothing else in the kernel, so the
# reference counting and the handle-table rules (lowest free slot, EMFILE,
# type checks, fork and execve) are checked here in milliseconds.
set -u
cd "$(dirname "$0")/.."

OUT=${OUT:-/tmp/objtest_host}
mkdir -p "$(dirname "$OUT")"

g++ -std=c++17 -g -O1 -fno-strict-aliasing \
    -Wall -Wextra -Wno-unused-parameter \
    -o "$OUT" \
    src/kernel/obj/object.cpp \
    tools/objhost/runner.cpp || { echo "BUILD FAILED"; exit 1; }

"$OUT"
