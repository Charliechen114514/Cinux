# user/CMakeLists.txt - User-space program compilation

set(USER_COMPILE_FLAGS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -fno-stack-protector
    -mcmodel=small
    -mno-red-zone
    -std=c++23
    -Wall
    -Wextra
    -O2
)

# ---- User libc (static library) ----
add_library(user_libc STATIC libc/syscall.cpp)
target_compile_options(user_libc PRIVATE ${USER_COMPILE_FLAGS})
target_compile_definitions(user_libc PRIVATE __USER__)
target_include_directories(user_libc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}
)

# ---- User program: hello ----
add_executable(user_hello programs/hello.cpp)
target_compile_options(user_hello PRIVATE ${USER_COMPILE_FLAGS})
target_compile_definitions(user_hello PRIVATE __USER__)
target_link_options(user_hello PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -nostdlib
    -static
    -no-pie
)
target_link_libraries(user_hello PRIVATE user_libc)
set_target_properties(user_hello PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "hello"
)

# ---- Post-build: ELF -> flat binary ----
set(USER_HELLO_ELF $<TARGET_FILE:user_hello>)
set(USER_HELLO_BIN "${CMAKE_BINARY_DIR}/user/hello.bin")

add_custom_command(TARGET user_hello POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${USER_HELLO_ELF} ${USER_HELLO_BIN}
    COMMENT "Converting user hello to flat binary"
    VERBATIM
)

# ---- Embed flat binary into linkable .o ----
set(USER_BINARY_OBJ "${CMAKE_BINARY_DIR}/user/user_binary.o")

add_custom_command(
    OUTPUT ${USER_BINARY_OBJ}
    COMMAND ${CMAKE_COMMAND} -E env LC_ALL=C
        ${CMAKE_LINKER} -r -b binary
        -o ${USER_BINARY_OBJ}
        hello.bin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/user
    DEPENDS user_hello ${USER_HELLO_BIN}
    COMMENT "Creating linkable object from user binary"
    VERBATIM
)

add_custom_target(user_binary_obj DEPENDS ${USER_BINARY_OBJ})
