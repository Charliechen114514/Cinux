set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_FLAGS_INIT "
    -ffreestanding
    -fno-stack-protector
    -mno-red-zone
    -mcmodel=kernel
    -Wall
    -Wextra
")

set(CMAKE_CXX_FLAGS_INIT ${CMAKE_C_FLAGS_INIT} "
    -fno-exceptions
    -fno-rtti
    -std=c++23
")

set(CMAKE_ASM_FLAGS_INIT "-Wa,--divide")

set(CMAKE_EXE_LINKER_FLAGS_INIT "
    -nostdlib                
    -static                  
    -Wl,--build-id=none      
")

set(CMAKE_FIND_ROOT_PATH "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
