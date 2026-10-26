add_executable(vbrecomp
    src/main.cpp
    src/analysis.cpp
    src/codegen.cpp
    src/config.cpp
    src/rom.cpp
    src/v810_decoder.cpp)

if(MSVC)
    target_compile_options(vbrecomp PRIVATE /W4)
else()
    target_compile_options(vbrecomp PRIVATE -Wall -Wextra)
endif()
