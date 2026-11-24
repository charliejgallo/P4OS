cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vision)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# The app (vision.so: the camera, the screens, the codes) and its engine
# apart: esp-dl, C++ and about a megabyte of code, is the module
# build/modules/engine.so, which tools/install_apps.sh puts in the card's
# apps/vision/ and the app opens with aos_hal_module_open() only while a
# detector is chosen (main/ve_api.h). The engine's library is
# engine/board.cmake; the module is linked and stripped as project_so()
# links an app, like the Emulador's cores.
include(${CMAKE_CURRENT_LIST_DIR}/engine/board.cmake)

include(elf_loader)
project_so(vision)

set(_mod_dir ${CMAKE_BINARY_DIR}/modules)
file(MAKE_DIRECTORY ${_mod_dir})
set(_mod_so ${_mod_dir}/engine.so)
add_custom_command(OUTPUT ${_mod_so}
    COMMAND ${CMAKE_C_COMPILER} ${so_link_flags} -o ${_mod_so}
            ${CMAKE_BINARY_DIR}/engine/libve_engine.a -Wl,--whole-archive -Wl,--no-whole-archive
            -Wl,--undefined=ve_module_api -Wl,--allow-shlib-undefined
    COMMAND ${CMAKE_STRIP_SO} ${so_strip_flags} ${_mod_so}
    DEPENDS ve_engine
    COMMENT "Build the module engine.so"
    VERBATIM)
add_custom_target(vision_engine ALL DEPENDS ${_mod_so})
add_dependencies(so vision_engine)
