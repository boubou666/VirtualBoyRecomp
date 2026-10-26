find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
    endif()
    if(NOT SDL2_FOUND)
        message(FATAL_ERROR "SDL2 not found. Install the SDL2 development package "
                            "(or set SDL2_DIR / CMAKE_PREFIX_PATH).")
    endif()
    add_library(SDL2::SDL2 ALIAS PkgConfig::SDL2)
endif()

add_library(vbrt STATIC
    src/cpu.cpp
    src/host_sdl.cpp
    src/hwctl.cpp
    src/memory.cpp
    src/system.cpp
    src/vip.cpp
    src/vsu.cpp)

target_include_directories(vbrt PUBLIC include PRIVATE src)

# SDL2main provides the platform entry point (WinMain on Windows) for the game's main().
if(TARGET SDL2::SDL2main)
    target_link_libraries(vbrt PUBLIC SDL2::SDL2main)
endif()
if(TARGET SDL2::SDL2)
    target_link_libraries(vbrt PUBLIC SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(vbrt PUBLIC SDL2::SDL2-static)
endif()

if(MSVC)
    target_compile_options(vbrt PRIVATE /W4)
else()
    target_compile_options(vbrt PRIVATE -Wall -Wextra)
endif()
