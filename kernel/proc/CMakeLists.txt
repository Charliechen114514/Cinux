target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/process_new.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/task_builder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fork.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/execve.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/elf_types.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/init.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pid.cpp
)
