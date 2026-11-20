#!/bin/sh
# Command line build of the firmware.
#
# The arm-none-eabi compiler is looked up here, so a fresh shell (or a CubeIDE
# update that moves the bundled toolchain) does not break the build:
#   1. $ARM_TOOLCHAIN_BIN, then arm-none-eabi-gcc in PATH
#   2. STM32CubeCLT
#   3. the toolchain bundled with STM32CubeIDE
# When several are installed the newest compiler wins.
#
#   tools/build.sh                            # build into build/
#   tools/build.sh --clean                    # drop build/ first
#   tools/build.sh --debug                    # + USB-PD trace on USART1
#   tools/build.sh --clean -DLCD_SIZE=240X240 # extra arguments go to cmake
#   BUILD_DIR=out tools/build.sh              # custom build directory
#
# Needs cmake 3.29 or newer and ninja (optional, make works as well).
set -e

cd "$(dirname "$0")/.."

BUILD_DIR=${BUILD_DIR:-build}
CLEAN=0
CMAKE_ARGS=

usage() {
    awk 'NR > 1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --debug) CMAKE_ARGS="$CMAKE_ARGS -DSTUSB_DEBUG=ON"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) break ;;
    esac
done

compiler_version() {
    "$1" -dumpversion 2>/dev/null || echo 0
}

# One "<version> <path>" line per usable compiler; the caller sorts them.
find_toolchains() {
    if [ -n "$ARM_TOOLCHAIN_BIN" ] && [ -x "$ARM_TOOLCHAIN_BIN/arm-none-eabi-gcc" ]; then
        echo "$(compiler_version "$ARM_TOOLCHAIN_BIN/arm-none-eabi-gcc") $ARM_TOOLCHAIN_BIN/arm-none-eabi-gcc"
    fi

    if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
        gcc=$(command -v arm-none-eabi-gcc)
        echo "$(compiler_version "$gcc") $gcc"
    fi

    for gcc in /Applications/STM32CubeCLT*/GNU-tools-for-StM32/bin/arm-none-eabi-gcc \
               /Applications/STM32CubeCLT*/GNU-tools-for-STM32/bin/arm-none-eabi-gcc \
               /opt/st/stm32cubeclt*/GNU-tools-for-STM32/bin/arm-none-eabi-gcc \
               "$HOME"/st/stm32cubeclt*/GNU-tools-for-STM32/bin/arm-none-eabi-gcc; do
        [ -x "$gcc" ] && echo "$(compiler_version "$gcc") $gcc"
    done

    for gcc in /Applications/STM32CubeIDE*.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin/arm-none-eabi-gcc; do
        [ -x "$gcc" ] && echo "$(compiler_version "$gcc") $gcc"
    done
}

GCC=$(find_toolchains | sort -n | tail -1 | cut -d' ' -f2)

if [ -z "$GCC" ]; then
    cat <<'EOF'
no arm-none-eabi-gcc found - install one of:
  * STM32CubeIDE (bundled toolchain, used automatically)
  * STM32CubeCLT
  * a standalone toolchain, e.g. brew install --cask gcc-arm-embedded
or point ARM_TOOLCHAIN_BIN at the directory that holds arm-none-eabi-gcc
EOF
    exit 1
fi

command -v cmake >/dev/null 2>&1 || { echo "cmake not found - brew install cmake"; exit 1; }

CMAKE_VERSION=$(cmake --version | sed -n '1s/[^0-9]*\([0-9][0-9.]*\).*/\1/p')
CMAKE_MAJOR=${CMAKE_VERSION%%.*}
CMAKE_REST=${CMAKE_VERSION#*.}
CMAKE_MINOR=${CMAKE_REST%%.*}

if [ "$CMAKE_MAJOR" -lt 3 ] || { [ "$CMAKE_MAJOR" -eq 3 ] && [ "$CMAKE_MINOR" -lt 29 ]; }; then
    echo "cmake $CMAKE_VERSION is too old - the project needs 3.29 or newer (brew upgrade cmake)"
    exit 1
fi

[ "$CLEAN" = "1" ] && rm -rf "$BUILD_DIR"

GENERATOR=
command -v ninja >/dev/null 2>&1 && GENERATOR="-G Ninja"

echo "compiler: $GCC ($("$GCC" -dumpversion))"
echo "cmake:    $CMAKE_VERSION ($(command -v cmake))"

cmake -S . -B "$BUILD_DIR" -DCMAKE_C_COMPILER="$GCC" $GENERATOR $CMAKE_ARGS "$@"
cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "image:    $BUILD_DIR/AxxSolder.bin"
