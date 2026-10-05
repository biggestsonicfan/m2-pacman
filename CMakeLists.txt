cmake_minimum_required(VERSION 3.20)

# m2-pacman: Pac-Man on the Sega Model 2B, built against m2-sdk (the SDK's headers, i960
# boot/IRQ assembly, linker script and ROM splitter). The game, its Z80 core, the SCSP
# relay and the tools are all here.
#
#   cmake -G "Unix Makefiles" -B build -DM2_GAME=pacman     (pacman, pacman_geo, pacman_web, puckman)
#   make -C build -j2                                       -> roms/<M2_GAME>/
#
# m2-sdk is expected beside this checkout (-DM2_SDK=<path> otherwise); its i960-elf
# toolchain file is used unless you pass your own -DCMAKE_TOOLCHAIN_FILE.
set(M2_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../m2-sdk" CACHE PATH "Path to the m2-sdk checkout")
if(NOT EXISTS "${M2_SDK}/src/m2.h")
    message(FATAL_ERROR "m2-sdk not found at ${M2_SDK} (set -DM2_SDK=<path>)")
endif()
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${M2_SDK}/toolchain-i960-elf.cmake")
endif()

project(m2_pacman C ASM)

set(M2_GAME "pacman" CACHE STRING "Which src/<game>.c to build (pacman pacman_geo pacman_web puckman)")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/${M2_GAME}.c")
    message(FATAL_ERROR "SOURCE NOT FOUND: src/${M2_GAME}.c")
endif()

# i960 startup + IRQ assembly (from the SDK; GNU syntax, assembled -AKB via the gcc
# driver). kx_init.o(.text) is forced first by the linker script, so order is free.
set(M2_STARTUP_ASM
    ${M2_SDK}/src/kx_init.s
    ${M2_SDK}/src/kx_ftbl.s
    ${M2_SDK}/src/i_table.s
    ${M2_SDK}/src/i_handle.s)

# Bare-metal ELF: game + SDK startup, NO libc/crt0. src/ first: the generated ROM and
# recompiled-code headers live there; the SDK's src/ has m2*.h and the cpres*.h firmware.
add_executable(game.elf ${M2_STARTUP_ASM} src/${M2_GAME}.c)
target_include_directories(game.elf PRIVATE src "${M2_SDK}/src")
target_compile_options(game.elf PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2>)
# a lone game .c: m2.h's boot stubs must be global (see the SDK's CMakeLists.txt)
target_compile_definitions(game.elf PRIVATE $<$<COMPILE_LANGUAGE:C>:M2_API=>)
set(PAC_DEFS "" CACHE STRING "Extra -D for the game (e.g. PAC_NO_PANEL)")
if(PAC_DEFS)
    target_compile_definitions(game.elf PRIVATE ${PAC_DEFS})
endif()
set_target_properties(game.elf PROPERTIES SUFFIX "")
target_link_options(game.elf PRIVATE
    -nostdlib
    "-T${M2_SDK}/lib/testlinkrom_elf.ld"
    "-Wl,-Map=game.map")

# Soft-float (no i960 hardware FP), the SDK's default; -DM2_SOFTFLOAT=OFF for MAME only.
include("${M2_SDK}/cmake/m2sdk.cmake")
if(M2_SOFTFLOAT)
    m2sdk_softfloat(game.elf)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# m2emulator masks shift counts to 5 bits; GCC's i960 backend computes `x == 0` as
# `shro x,1,y` (1 >> x), which then reads every non-zero multiple of 32 as zero.
# tools/emu_shro.py rewrites those shifts in the compiler's assembly into sequences that
# give the same result on silicon, MAME and m2emulator. -DM2_EMU_SHRO=OFF to skip it.
option(M2_EMU_SHRO "Rewrite GCC's shro equality idiom for m2emulator" ON)
if(M2_EMU_SHRO)
    set_target_properties(game.elf PROPERTIES C_COMPILER_LAUNCHER
        "${Python3_EXECUTABLE};${CMAKE_SOURCE_DIR}/tools/emu_shro.py")
endif()

# roms/<game>/: the program EPROM pair (the SDK's stfbin2rom.py), the sound program EPROM
# (snd/scsp_passthru.bin: the i960 drives the SCSP), so sfight + these three files is a
# complete set, and the flat image tools/m2_load.lua boots on a stock sfight romset.
# Always-run, so a deleted roms/ is regenerated even when game.elf is unchanged.
set(ROM_DIR "${CMAKE_SOURCE_DIR}/roms/${M2_GAME}")
add_custom_target(rom ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ROM_DIR}"
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:game.elf> game.bin
    COMMAND ${Python3_EXECUTABLE} "${M2_SDK}/tools/stfbin2rom.py"
            --input game.bin --output "${ROM_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/snd2rom.py"
            "${CMAKE_SOURCE_DIR}/snd/scsp_passthru.bin" "${ROM_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy game.bin "${ROM_DIR}/game.bin"
    COMMAND ${CMAKE_COMMAND} -E echo
            "BUILD OK (${M2_GAME}):  roms/${M2_GAME}/: epr-19001.15  epr-19002.16  epr-19021.31 (sound)  game.bin"
    DEPENDS game.elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    VERBATIM)

# make -C build show-asm   (disassemble for real i960 mnemonics)
add_custom_target(show-asm
    COMMAND ${CMAKE_OBJDUMP} -d $<TARGET_FILE:game.elf>
    DEPENDS game.elf
    COMMENT "Disassembling ${M2_GAME} game.elf for i960-elf"
    VERBATIM)

# make -C build snd   (re-assemble the sound board's 68000 program, snd/scsp_passthru.bin;
# the .bin is committed, so this is only needed after editing snd/scsp_passthru.s)
find_program(M68K_AS NAMES m68k-linux-gnu-as m68k-elf-as)
find_program(M68K_LD NAMES m68k-linux-gnu-ld m68k-elf-ld)
if(M68K_AS AND M68K_LD)
    add_custom_target(snd
        COMMAND ${M68K_AS} -m68000 -o scsp_passthru.o "${CMAKE_SOURCE_DIR}/snd/scsp_passthru.s"
        COMMAND ${M68K_LD} -Ttext=0x600000 --oformat binary
                -o "${CMAKE_SOURCE_DIR}/snd/scsp_passthru.bin" scsp_passthru.o
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Assembling snd/scsp_passthru.s (68000 sound program)"
        VERBATIM)
endif()
