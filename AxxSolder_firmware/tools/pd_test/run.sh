#!/bin/sh
# Host-side check of the STUSB4500 register decoding (stusb4500_regs.h).
# No STM32 toolchain or hardware needed: the macros are plain arithmetic.
set -e

cd "$(dirname "$0")/../.."

CC=${CC:-cc}
OUT=${OUT:-/tmp/axx_pd_test}

$CC -std=c11 -O2 -Wall -Wextra \
    -I Core/Inc \
    tools/pd_test/main.c \
    -o "$OUT"

"$OUT"
