cmake_minimum_required(VERSION 3.16)
project(VirtualBoyRecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(VB_GAME_CONFIG "" CACHE FILEPATH
    "Per-game recompiler config (TOML). When set, the game is recompiled and built.")
set(VB_ROM "" CACHE FILEPATH "ROM image; overrides the 'rom' entry of the game config")

# Must match the number of rec_N.cpp files the recompiler writes.
set(VB_CHUNKS 16)

add_subdirectory(recompiler)
add_subdirectory(runtime)

if(NOT VB_GAME_CONFIG)
    message(STATUS "No game configured: building vbrecomp and the runtime only. "
                   "Pass -DVB_GAME_CONFIG=<game.toml> to recompile a game.")
    return()
endif()
get_filename_component(VB_GAME_CONFIG "${VB_GAME_CONFIG}" ABSOLUTE)
if(NOT EXISTS "${VB_GAME_CONFIG}")
    message(FATAL_ERROR "Game config not found: ${VB_GAME_CONFIG}")
endif()

# ---- Resolve the ROM path (config entry, relative to the config file) ----
get_filename_component(_cfg_dir "${VB_GAME_CONFIG}" DIRECTORY)
get_filename_component(VB_GAME_NAME "${VB_GAME_CONFIG}" NAME_WE)
if(VB_ROM)
    set(_rom "${VB_ROM}")
else()
    file(STRINGS "${VB_GAME_CONFIG}" _rom_line REGEX "^[ \t]*rom[ \t]*=")
    if(NOT _rom_line)
        message(FATAL_ERROR "${VB_GAME_CONFIG} has no 'rom' entry; set -DVB_ROM=<file.vb>")
    endif()
    string(REGEX REPLACE "^[ \t]*rom[ \t]*=[ \t]*\"([^\"]*)\".*" "\\1" _rom "${_rom_line}")
    if(NOT IS_ABSOLUTE "${_rom}")
        set(_rom "${_cfg_dir}/${_rom}")
    endif()
endif()
get_filename_component(_rom "${_rom}" ABSOLUTE)
if(NOT EXISTS "${_rom}")
    message(FATAL_ERROR "ROM not found: ${_rom}\nPlace your ROM there or pass -DVB_ROM=<file.vb>.")
endif()
message(STATUS "Game: ${VB_GAME_NAME}, ROM: ${_rom}")

# ---- Recompile at build time ----
set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/${VB_GAME_NAME}")
set(_gen_files "${_gen_dir}/rec_funcs.h" "${_gen_dir}/rec_table.cpp")
math(EXPR _last "${VB_CHUNKS} - 1")
foreach(i RANGE ${_last})
    list(APPEND _gen_files "${_gen_dir}/rec_${i}.cpp")
endforeach()

add_custom_command(
    OUTPUT ${_gen_files}
    COMMAND vbrecomp "${VB_GAME_CONFIG}" --rom "${_rom}" --out "${_gen_dir}" --chunks ${VB_CHUNKS}
    DEPENDS vbrecomp "${VB_GAME_CONFIG}" "${_rom}"
    COMMENT "Recompiling ${VB_GAME_NAME}"
    VERBATIM)

add_executable(${VB_GAME_NAME} ${_gen_files} runtime/src/main.cpp)
target_include_directories(${VB_GAME_NAME} PRIVATE runtime/src "${_gen_dir}")
target_link_libraries(${VB_GAME_NAME} PRIVATE vbrt)
target_compile_definitions(${VB_GAME_NAME} PRIVATE VB_DEFAULT_ROM="${_rom}")
if(MSVC)
    target_compile_options(${VB_GAME_NAME} PRIVATE /bigobj)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Generated functions are huge; the cheaper optimizations keep build times sane.
    set_source_files_properties(${_gen_files} PROPERTIES COMPILE_OPTIONS "-O1;-fno-strict-aliasing")
    if(MINGW)
        target_compile_options(${VB_GAME_NAME} PRIVATE -Wa,-mbig-obj)
    endif()
endif()
set_target_properties(${VB_GAME_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
