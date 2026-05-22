target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/pit/pit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/serial/serial.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/keyboard/keyboard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/framebuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/video/font_data.S
    ${CMAKE_CURRENT_SOURCE_DIR}/video/console.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pci/pci.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ahci/ahci.cpp
)

if(CINUX_GUI)
    target_sources(big_kernel_common PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/canvas.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mouse.cpp
    )
endif()
