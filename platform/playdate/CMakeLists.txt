# Playdate build for open8 (milestone 1 / Phase 0).
#
# Self-contained: reuses the open8 sources under ../../src compiled with the SDL
# shim and OPEN8_PLATFORM_PLAYDATE. The root CMakeLists.txt (the SDL build) is
# untouched.
#
# Build (simulator):
#   cd platform/playdate && cmake -B build -G "Unix Makefiles" && cmake --build build
# Build (device):
#   cmake -B build-device -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake
#   cmake --build build-device
cmake_minimum_required(VERSION 3.19)
set(CMAKE_C_STANDARD 11)

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

set(CMAKE_CONFIGURATION_TYPES "Debug;Release")

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
	${OPEN8_SRC}/auxiliary.c
	${OPEN8_SRC}/p8scii.c
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

if (TOOLCHAIN STREQUAL "armgcc")
	add_executable(${PLAYDATE_GAME_DEVICE} ${OPEN8_SOURCES})
	set(OPEN8_TARGET ${PLAYDATE_GAME_DEVICE})
else()
	add_library(${PLAYDATE_GAME_NAME} SHARED ${OPEN8_SOURCES})
	set(OPEN8_TARGET ${PLAYDATE_GAME_NAME})
endif()

target_compile_definitions(${OPEN8_TARGET} PRIVATE OPEN8_PLATFORM_PLAYDATE=1)

target_include_directories(${OPEN8_TARGET} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}          # generated/cart_celeste.h
	${CMAKE_CURRENT_SOURCE_DIR}/shim     # SDL3/SDL.h shim, pdshim.h
	${OPEN8_SRC}                         # api.h, core.h, memory.h, lexaloffle/, misc/
	${OPEN8_SRC}/z8lua)                  # lua.h and friends

# Silence the upstream sources' expected noise so the real warnings stand out.
target_compile_options(${OPEN8_TARGET} PRIVATE -Wno-unused-parameter -Wno-sign-compare)

include(${SDK}/C_API/buildsupport/playdate_game.cmake)
