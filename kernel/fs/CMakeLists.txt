target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ramdisk.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2_common.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2_init.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2_inode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2_block.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ext2_directory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs_mount.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/path.cpp
)
