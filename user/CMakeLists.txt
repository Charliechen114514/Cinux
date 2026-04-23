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
add_library(user_libc STATIC
    libc/syscall.cpp
    libc/string.cpp
    libc/printf.cpp
)
target_compile_options(user_libc PRIVATE ${USER_COMPILE_FLAGS})
target_compile_definitions(user_libc PRIVATE __USER__)
target_include_directories(user_libc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}
)

# ---- User program: shell ----
add_executable(user_shell
    programs/shell/main.cpp
    programs/shell/cmd_echo.cpp
    programs/shell/cmd_help.cpp
    programs/shell/cmd_clear.cpp
    programs/shell/cmd_cat.cpp
    programs/shell/cmd_ls.cpp
    programs/shell/cmd_touch.cpp
    programs/shell/cmd_mkdir.cpp
    programs/shell/cmd_rm.cpp
    programs/shell/cmd_rmdir.cpp
    programs/shell/cmd_cd.cpp
    programs/shell/cmd_pwd.cpp
    programs/shell/cmd_stat.cpp
)
target_compile_options(user_shell PRIVATE ${USER_COMPILE_FLAGS})
target_compile_definitions(user_shell PRIVATE __USER__)
target_link_options(user_shell PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -nostdlib
    -static
    -no-pie
)
target_link_libraries(user_shell PRIVATE user_libc)
set_target_properties(user_shell PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/user
    OUTPUT_NAME "shell"
)

# ---- Post-build: ELF -> flat binary ----
set(USER_SHELL_ELF $<TARGET_FILE:user_shell>)
set(USER_SHELL_BIN "${CMAKE_BINARY_DIR}/user/shell.bin")

add_custom_command(TARGET user_shell POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${USER_SHELL_ELF} ${USER_SHELL_BIN}
    COMMENT "Converting user shell to flat binary"
    VERBATIM
)

# ---- Embed flat binary into linkable .o ----
set(USER_BINARY_OBJ "${CMAKE_BINARY_DIR}/user/user_binary.o")

add_custom_command(
    OUTPUT ${USER_BINARY_OBJ}
    COMMAND ${CMAKE_COMMAND} -E env LC_ALL=C
        ${CMAKE_LINKER} -r -b binary
        -o ${USER_BINARY_OBJ}
        shell.bin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/user
    DEPENDS user_shell ${USER_SHELL_BIN}
    COMMENT "Creating linkable object from user binary"
    VERBATIM
)

add_custom_target(user_binary_obj DEPENDS ${USER_BINARY_OBJ})
