cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(infrarrojo)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# The header's API (aos_io.h: the IR receiver and LED), which common.cmake
# does not bring: this is the first card app that uses the pins.
idf_build_set_property(COMPILE_INCLUDE_DIRECTORIES "${AOS_ROOT}/components/aos_io/include" APPEND)
idf_build_set_property(INCLUDE_DIRECTORIES "${AOS_ROOT}/components/aos_io/include" APPEND)

# Genera infrarrojo.so en lugar de un firmware completo.
include(elf_loader)
project_so(infrarrojo)
