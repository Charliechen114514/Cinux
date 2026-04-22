# TODO: 设置 CMake 最低版本要求
# Cinux 要求 CMake 3.20+ 以支持现代 CMake 特性
# cmake_minimum_required(VERSION 3.20)
cmake_minimum_required(VERSION 4.1)

project(cinux 
    VERSION 0.1.0 
    LANGUAGES C CXX ASM)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_SOURCE_DIR}/cmake/toolchain-x86_64.cmake"
        CACHE FILEPATH "Toolchain file")
    message(STATUS "Using default toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
endif()

add_compile_options(
    -Wall
    -Wextra
)

# Export CompileCommands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

add_subdirectory(boot)
add_subdirectory(user)
add_subdirectory(kernel)

include(cmake/qemu.cmake)

if(CINUX_BUILD_TESTS)
    add_subdirectory(test)
endif()

message(STATUS "")
message(STATUS "=== Cinux Build Configuration ===")
message(STATUS "  Project:     ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "  Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler:  ${CMAKE_C_COMPILER}")
message(STATUS "  CXX Compiler:${CMAKE_CXX_COMPILER}")
message(STATUS "  ASM Compiler:${CMAKE_ASM_COMPILER}")
message(STATUS "  Toolchain:   ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "  Build tests: ${CINUX_BUILD_TESTS}")
message(STATUS "===================================")
message(STATUS "")
