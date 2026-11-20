#!/bin/sh
# Flash build/AxxSolder.bin over the STM32 ROM DFU bootloader.
#
# Enter DFU first: hold SW_3 (BOOT0, the rightmost button) while plugging the
# USB cable in. Then run:
#
#   tools/flash_dfu.sh                # write the application region
#   tools/flash_dfu.sh --erase-all    # mass erase first (settings, profiles and
#                                     # the FAT volume in the FS region are lost)
#   tools/flash_dfu.sh --wait 60      # wait up to 60 s for the DFU device, then
#                                     # flash (hold SW_3 while plugging in USB)
#
# FLASHER=dfu|cli forces one flasher; default is dfu-util when installed.
#
# Requires dfu-util (brew install dfu-util) or STM32CubeProgrammer's CLI.
set -e

cd "$(dirname "$0")/.."

IMAGE=${IMAGE:-build/AxxSolder.bin}
ERASE_ALL=0
WAIT_S=0
FLASHER=${FLASHER:-}

while [ $# -gt 0 ]; do
    case "$1" in
        --erase-all) ERASE_ALL=1; shift ;;
        --wait) WAIT_S=${2:-60}; shift 2 ;;
        --wait=*) WAIT_S=${1#*=}; shift ;;
        -h|--help) awk 'NR > 1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
        *) echo "unknown argument: $1"; exit 1 ;;
    esac
done

[ -f "$IMAGE" ] || { echo "missing $IMAGE - build the firmware first"; exit 1; }

# STM32CubeProgrammer ships with STM32CubeIDE and with the standalone
# installer; add its CLI to PATH when it is not installed globally (that also
# makes FLASHER=cli work on a machine that has dfu-util).
if ! command -v STM32_Programmer_CLI >/dev/null 2>&1; then
    for dir in /Applications/STM32CubeIDE*.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.*/tools/bin \
               /Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin \
               "$HOME"/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin \
               /opt/st/stm32cubeprog/bin; do
        if [ -x "$dir/STM32_Programmer_CLI" ]; then
            PATH="$dir:$PATH"
            export PATH
            break
        fi
    done
fi

# Is the STM32 visible in DFU mode? Works with either flasher.
device_present() {
    if command -v dfu-util >/dev/null 2>&1; then
        dfu-util -l 2>/dev/null | grep -q '0483:df11'
    else
        ! STM32_Programmer_CLI -l usb 2>/dev/null | grep -q 'No STM32 device'
    fi
}

# --wait: plug the station in while this runs.
case "$WAIT_S" in
    ''|*[!0-9]*) echo "--wait needs a number of seconds"; exit 1 ;;
esac

if [ "$WAIT_S" != "0" ]; then
    echo "waiting up to ${WAIT_S}s for the STM32 in DFU mode"
    echo "hold SW_3 (BOOT0) while plugging in the USB cable"
    deadline=$(( $(date +%s) + WAIT_S ))
    until device_present; do
        if [ "$(date +%s)" -ge "$deadline" ]; then
            echo "no STM32 DFU device appeared"
            exit 2
        fi
        sleep 1
    done
    echo "DFU device found"
fi

# Pick the flasher: dfu-util first, STM32CubeProgrammer CLI as the fallback.
# FLASHER=dfu|cli forces one of them. Note that dfu-util's DFU page erase is
# noticeably less reliable on this board than the CubeProgrammer path - if the
# download aborts with "ERASE_PAGE"/"SET_ADDRESS" errors, retry with
# FLASHER=cli.
if [ -n "$FLASHER" ]; then
    :
elif command -v dfu-util >/dev/null 2>&1; then
    FLASHER=dfu
elif command -v STM32_Programmer_CLI >/dev/null 2>&1; then
    FLASHER=cli
else
    echo "need dfu-util (brew install dfu-util) or STM32CubeProgrammer CLI"
    exit 3
fi

case "$FLASHER" in
dfu)
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
    ;;
cli)
    if STM32_Programmer_CLI -l usb 2>/dev/null | grep -q 'No STM32 device'; then
        echo "no STM32 DFU device found - hold SW_3 (BOOT0) while plugging in USB"
        exit 2
    fi

    # No -rst: a reset is only available over SWD, the ROM bootloader ignores
    # it and the CLI then exits with an error even though the write succeeded.
    if [ "$ERASE_ALL" = "1" ]; then
        STM32_Programmer_CLI -c port=USB1 -e all -w "$IMAGE" 0x08000000 -v
    else
        STM32_Programmer_CLI -c port=USB1 -w "$IMAGE" 0x08000000 -v
    fi
    ;;
*)
    echo "FLASHER must be dfu or cli"
    exit 1
    ;;
esac

echo "done - power cycle the station"

