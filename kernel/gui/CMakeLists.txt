#
# kernel/gui/CMakeLists.txt
#
# @brief GUI subsystem CMake configuration
#
# Contains GUI-only components: event queue, window manager, etc.
#

# Event queue (ring buffer for mouse/keyboard events)
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/event.cpp
)

# Window class (off-screen canvas, title bar, hit testing)
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/window.cpp
)

# Window manager (Z-ordering, compositing, drag)
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/window_manager.cpp
)

# GUI initialisation (mouse driver, WM setup, PIT tick callback)
target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/gui_init.cpp
)
