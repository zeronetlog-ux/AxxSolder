#!/bin/sh
# Flash build/AxxSolder.bin over the STM32 ROM DFU bootloader.
#
# Enter DFU first: hold SW_3 (BOOT0, the rightmost button) while plugging the
# USB cable in. Then run:
#
#   tools/flash_dfu.sh                # write the application region
#   tools/flash_dfu.sh --erase-all    # mass erase first (settings, profiles and
#                                     # the FAT volume in the FS region are lost)
#
# Requires dfu-util (brew install dfu-util) or STM32CubeProgrammer's CLI.
set -e

cd "$(dirname "$0")/.."

IMAGE=${IMAGE:-build/AxxSolder.bin}
ERASE_ALL=0
[ "$1" = "--erase-all" ] && ERASE_ALL=1

[ -f "$IMAGE" ] || { echo "missing $IMAGE - build the firmware first"; exit 1; }

if command -v dfu-util >/dev/null 2>&1; then
    # Several DFU-capable devices may be present, so select the STM32 by serial.
    serial=$(dfu-util -l 2>/dev/null \
             | sed -n 's/.*DFU: \[0483:df11\].*serial="\([^"]*\)".*/\1/p' \
             | sort -u | head -1)

    if [ -z "$serial" ]; then
        echo "no STM32 DFU device found - hold SW_3 (BOOT0) while plugging in USB"
        exit 2
    fi

    if [ "$ERASE_ALL" = "1" ]; then
        dfu-util -S "$serial" -a 0 -s 0x08000000:mass-erase:force
    fi

    dfu-util -S "$serial" -a 0 -s 0x08000000:leave -D "$IMAGE"
    echo "done - power cycle the station"
elif command -v STM32_Programmer_CLI >/dev/null 2>&1; then
    if [ "$ERASE_ALL" = "1" ]; then
        STM32_Programmer_CLI -c port=USB1 -e all -w "$IMAGE" 0x08000000 -v -rst
    else
        STM32_Programmer_CLI -c port=USB1 -w "$IMAGE" 0x08000000 -v -rst
    fi
    echo "done - power cycle the station"
else
    echo "need dfu-util (brew install dfu-util) or STM32CubeProgrammer CLI"
    exit 3
fi
