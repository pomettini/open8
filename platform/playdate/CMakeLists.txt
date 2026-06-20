# Playdate build for open8.
#
# Self-contained: reuses the open8 sources under ../../src compiled with the SDL
# shim and OPEN8_PLATFORM_PLAYDATE. The root CMakeLists.txt (the SDL build) is
# untouched.
#
# Build (simulator):
#   cmake -S platform/playdate -B platform/playdate/build -G "Unix Makefiles"
#   cmake --build platform/playdate/build
# Build (device):
#   cmake -S platform/playdate -B platform/playdate/build-release -G "Unix Makefiles" \
#     -DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake
#   cmake --build platform/playdate/build-release
#
# Single-config generators default to Release. An empty CMAKE_BUILD_TYPE
# silently produced the original unoptimized benchmark binary.
cmake_minimum_required(VERSION 3.19)
set(CMAKE_C_STANDARD 11)

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OPEN8_PROFILE_LOAD "Count every Lua opcode and C call" OFF)
option(OPEN8_PROFILE_TOOLS "Build runtime skip-fill profiling controls" OFF)
option(OPEN8_PROFILE_API "Count selected API calls separately for update/draw" OFF)
option(OPEN8_ARENA_ALLOCATOR "Use the experimental Lua arena allocator" OFF)
option(OPEN8_VM_GOTO "Token-threaded (computed-goto) VM dispatch — re-test at Release" OFF)
option(OPEN8_VM_LCF_FAST "Guarded fast path for Lua calls to light C functions" OFF)
option(OPEN8_VM_COMPACT "Compile the hot Lua interpreter core for minimum code size" OFF)
option(OPEN8_VM_DTCM_PREFLIGHT "Report VM/DTCM placement without copying or executing code" OFF)
option(OPEN8_VM_DTCM_WATERMARK "Paint the unused stack gap above the proposed DTCM pool" OFF)
option(OPEN8_VM_DTCM_EXEC "Copy the compact Lua VM core and selected helpers to guarded executable DTCM" OFF)
option(OPEN8_GFX_FAST "Enable compact sprite-pair LUT and solid-rectangle fast paths" OFF)

if (OPEN8_VM_DTCM_PREFLIGHT OR OPEN8_VM_DTCM_WATERMARK OR OPEN8_VM_DTCM_EXEC)
	set(OPEN8_VM_COMPACT ON)
	set(OPEN8_VM_DTCM_LAYOUT ON)
endif()
if (OPEN8_VM_DTCM_WATERMARK)
	set(OPEN8_VM_DTCM_PREFLIGHT ON)
endif()

set(ENVSDK $ENV{PLAYDATE_SDK_PATH})
if (NOT "${ENVSDK}" STREQUAL "")
	file(TO_CMAKE_PATH ${ENVSDK} SDK)
else()
	execute_process(
		COMMAND bash -c "egrep '^\\s*SDKRoot' $HOME/.Playdate/config"
		COMMAND head -n 1
		COMMAND cut -c9-
		OUTPUT_VARIABLE SDK
		OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if (NOT EXISTS ${SDK})
	message(FATAL_ERROR "SDK Path not found; set PLAYDATE_SDK_PATH")
	return()
endif()

set(PLAYDATE_GAME_NAME open8)
set(PLAYDATE_GAME_DEVICE open8_DEVICE)

project(${PLAYDATE_GAME_NAME} C ASM)

set(OPEN8_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

set(OPEN8_SOURCES
	# Playdate backend.
	${CMAKE_CURRENT_SOURCE_DIR}/pd_main.c
	${CMAKE_CURRENT_SOURCE_DIR}/shim/pdshim.c
	# open8 core (SDL-free heart, compiled against the shim).
	${OPEN8_SRC}/api.c
	${OPEN8_SRC}/core.c
	${OPEN8_SRC}/memory.c
	${OPEN8_SRC}/p8_text.c
	${OPEN8_SRC}/auxiliary.c
	${OPEN8_SRC}/audio.c
	${OPEN8_SRC}/p8scii.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/pd-rom-picker/src/rom_picker.c
	${OPEN8_SRC}/lexaloffle/p8_compress.c
	${OPEN8_SRC}/lexaloffle/pxa_compress_snippets.c
	# z8lua VM.
	${OPEN8_SRC}/z8lua/lapi.c   ${OPEN8_SRC}/z8lua/lauxlib.c ${OPEN8_SRC}/z8lua/lbaselib.c
	${OPEN8_SRC}/z8lua/lcode.c  ${OPEN8_SRC}/z8lua/lcorolib.c ${OPEN8_SRC}/z8lua/lctype.c
	${OPEN8_SRC}/z8lua/ldblib.c ${OPEN8_SRC}/z8lua/ldebug.c  ${OPEN8_SRC}/z8lua/ldo.c
	${OPEN8_SRC}/z8lua/ldump.c  ${OPEN8_SRC}/z8lua/lfunc.c   ${OPEN8_SRC}/z8lua/lgc.c
	${OPEN8_SRC}/z8lua/linit.c  ${OPEN8_SRC}/z8lua/llex.c    ${OPEN8_SRC}/z8lua/lmem.c
	${OPEN8_SRC}/z8lua/lobject.c ${OPEN8_SRC}/z8lua/lopcodes.c ${OPEN8_SRC}/z8lua/lparser.c
	${OPEN8_SRC}/z8lua/lpico8lib.c ${OPEN8_SRC}/z8lua/lstate.c ${OPEN8_SRC}/z8lua/lstring.c
	${OPEN8_SRC}/z8lua/lstrlib.c ${OPEN8_SRC}/z8lua/ltable.c  ${OPEN8_SRC}/z8lua/ltablib.c
	${OPEN8_SRC}/z8lua/ltm.c    ${OPEN8_SRC}/z8lua/lundump.c ${OPEN8_SRC}/z8lua/lvm.c
	${OPEN8_SRC}/z8lua/lzio.c)

if (OPEN8_ARENA_ALLOCATOR)
	list(APPEND OPEN8_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/arena_alloc.c)
endif()
if (OPEN8_VM_DTCM_WATERMARK)
	list(APPEND OPEN8_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/dtcm_watermark.c)
endif()
if (OPEN8_VM_DTCM_EXEC)
	list(APPEND OPEN8_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/dtcm_exec.c)
endif()

if (TOOLCHAIN STREQUAL "armgcc")
	# Device build needs newlib syscall stubs. External carts use pd->file,
	# never POSIX stdio.
	add_executable(${PLAYDATE_GAME_DEVICE} ${OPEN8_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/shim/pd_syscalls.c)
	set(OPEN8_TARGET ${PLAYDATE_GAME_DEVICE})
else()
	add_library(${PLAYDATE_GAME_NAME} SHARED ${OPEN8_SOURCES})
	set(OPEN8_TARGET ${PLAYDATE_GAME_NAME})
endif()

target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_PLATFORM_PLAYDATE=1)
# Computed-goto regressed in the original -O0 build; re-testable at Release via
# -DOPEN8_VM_GOTO=ON now that the optimized luaV_execute is far smaller.
if (OPEN8_VM_GOTO)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_VM_GOTO=1)
endif()
if (OPEN8_VM_LCF_FAST)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_VM_LCF_FAST=1)
endif()
if (OPEN8_VM_COMPACT)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_VM_COMPACT=1)
endif()
if (OPEN8_VM_DTCM_LAYOUT)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_VM_DTCM_LAYOUT=1)
	if (TOOLCHAIN STREQUAL "armgcc")
		set_source_files_properties(${OPEN8_SRC}/z8lua/lvm.c PROPERTIES
			COMPILE_OPTIONS "-mlong-calls;-fno-lto")
	endif()
endif()
if (OPEN8_VM_DTCM_PREFLIGHT)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_VM_DTCM_PREFLIGHT=1)
endif()
if (OPEN8_VM_DTCM_WATERMARK)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_VM_DTCM_WATERMARK=1)
endif()
if (OPEN8_VM_DTCM_EXEC)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_VM_DTCM_EXEC=1)
endif()
if (OPEN8_GFX_FAST)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_GFX_FAST=1)
endif()
if (OPEN8_PROFILE_LOAD)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_PROFILE_LOAD=1)
endif()
if (OPEN8_PROFILE_TOOLS)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_PROFILE_TOOLS=1)
endif()
if (OPEN8_PROFILE_API)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_PROFILE_API=1)
endif()
if (OPEN8_ARENA_ALLOCATOR)
	target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_ARENA_ALLOC=1)
endif()

target_include_directories(${OPEN8_TARGET} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/shim     # SDL3/SDL.h shim, pdshim.h
	${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/pd-rom-picker/src
	${OPEN8_SRC}                         # api.h, core.h, memory.h, lexaloffle/, misc/
	${OPEN8_SRC}/z8lua)                  # lua.h and friends

# Silence the upstream sources' expected noise so the real warnings stand out.
target_compile_options(${OPEN8_TARGET} PRIVATE -Wno-unused-parameter -Wno-sign-compare)

message(STATUS "open8 Playdate build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "open8 load counters: ${OPEN8_PROFILE_LOAD}")
message(STATUS "open8 profiling tools: ${OPEN8_PROFILE_TOOLS}")
message(STATUS "open8 API counters: ${OPEN8_PROFILE_API}")
message(STATUS "open8 arena allocator: ${OPEN8_ARENA_ALLOCATOR}")
message(STATUS "open8 light-C call fast path: ${OPEN8_VM_LCF_FAST}")
message(STATUS "open8 compact VM core: ${OPEN8_VM_COMPACT}")
message(STATUS "open8 VM DTCM preflight: ${OPEN8_VM_DTCM_PREFLIGHT}")
message(STATUS "open8 VM DTCM stack watermark: ${OPEN8_VM_DTCM_WATERMARK}")
message(STATUS "open8 executable DTCM VM: ${OPEN8_VM_DTCM_EXEC}")
message(STATUS "open8 graphics fast paths: ${OPEN8_GFX_FAST}")

include(${SDK}/C_API/buildsupport/playdate_game.cmake)

if (OPEN8_VM_DTCM_LAYOUT AND TOOLCHAIN STREQUAL "armgcc")
	get_target_property(OPEN8_LINK_OPTIONS ${OPEN8_TARGET} LINK_OPTIONS)
	list(REMOVE_ITEM OPEN8_LINK_OPTIONS
		"-T${SDK}/C_API/buildsupport/link_map.ld")
	list(APPEND OPEN8_LINK_OPTIONS
		"-T${CMAKE_CURRENT_SOURCE_DIR}/link_map.ld")
	set_property(TARGET ${OPEN8_TARGET} PROPERTY LINK_OPTIONS "${OPEN8_LINK_OPTIONS}")
endif()
