#!/bin/sh
# Host-side test of the USB-storage data path (fs_disk + FatFS + settings file).
# No STM32 toolchain or hardware needed: the flash region is a RAM buffer.
set -e

cd "$(dirname "$0")/../.."

CC=${CC:-cc}
OUT=${OUT:-/tmp/axx_fs_test}

$CC -std=c11 -O2 -Wall \
    -I tools/fs_test/include \
    -I Core/Inc \
    -I Middlewares/Third_Party/FatFS/src \
    -include tools/fs_test/host_flash.h \
    -DFS_DISK_BASE='((uintptr_t)host_flash_region)' \
    -DFS_DISK_SECTORS=40u \
    tools/fs_test/main.c \
    Core/Src/fs_disk.c \
    Core/Src/settings_file.c \
    Middlewares/Third_Party/FatFS/src/ff.c \
    -o "$OUT"

"$OUT"
