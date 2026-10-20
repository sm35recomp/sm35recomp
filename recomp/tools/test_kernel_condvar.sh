#!/bin/bash
# Tests for runtime/kernel.c's mutexes and condition variables (tools/test_kernel_condvar.c): kernel.c on its own,
# with stubs for the rest of the runtime. Built with the address and undefined-behaviour sanitizers.
# Usage: tools/test_kernel_condvar.sh [case...]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build/kernel_test
clang -std=gnu11 -Wall -Werror -Wno-unused-function -O1 -g -pthread -fno-strict-aliasing -fsanitize=address,undefined \
    -Iruntime runtime/kernel.c tools/test_kernel_condvar.c -o build/kernel_test/test_kernel_condvar
build/kernel_test/test_kernel_condvar "$@"
