set(CMAKE_SYSTEM_NAME               Generic)
set(CMAKE_SYSTEM_PROCESSOR          arm)

# Some default GCC settings
# arm-none-eabi- must be part of path environment (or be provided by the IDE kit)
set(TOOLCHAIN_PREFIX                arm-none-eabi-)
set(FLAGS                           "-fdata-sections -ffunction-sections --specs=nano.specs -Wl,--gc-sections")
set(CPP_FLAGS                       "-fno-rtti -fno-exceptions -fno-threadsafe-statics")

# Define compiler settings.
# IDEs (CMake Tools / STM32 VS Code extension) hand over the full compiler path
# through -DCMAKE_C_COMPILER; keep it when present and fall back to a PATH
# lookup for plain command line builds.
if(NOT CMAKE_C_COMPILER)
    find_program(CMAKE_C_COMPILER NAMES ${TOOLCHAIN_PREFIX}gcc)
endif()

set(CMAKE_ASM_COMPILER              ${CMAKE_C_COMPILER})

# try_compile sub-projects (e.g. the compiler ABI check) do not inherit the
# compiler that the IDE passed on the command line, so forward it explicitly;
# otherwise those checks fall back to a PATH lookup and fail.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_C_COMPILER CMAKE_ASM_COMPILER)

# objcopy and size are invoked by the POST_BUILD steps in CMakeLists.txt; look
# for them next to the compiler first so the build does not depend on PATH.
get_filename_component(TOOLCHAIN_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
find_program(CMAKE_OBJCOPY NAMES ${TOOLCHAIN_PREFIX}objcopy HINTS ${TOOLCHAIN_BIN_DIR})
find_program(CMAKE_SIZE    NAMES ${TOOLCHAIN_PREFIX}size    HINTS ${TOOLCHAIN_BIN_DIR})

set(CMAKE_EXECUTABLE_SUFFIX_ASM     ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C       ".elf")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The optimization/code generation flags used to be part of the compiler
# definition above; keep them as initial flags so they apply both to compile
# and link steps, no matter how the compiler itself was discovered.
set(CMAKE_C_FLAGS_INIT              "${FLAGS}")
set(CMAKE_ASM_FLAGS_INIT            "${FLAGS}")
