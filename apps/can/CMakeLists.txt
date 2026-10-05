cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(can)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# The header's API (aos_io.h) is not among common.cmake's includes: added
# here, both for the component build and for project_so()'s own step.
idf_build_set_property(INCLUDE_DIRECTORIES "${AOS_ROOT}/components/aos_io/include" APPEND)
idf_build_set_property(COMPILE_INCLUDE_DIRECTORIES "${AOS_ROOT}/components/aos_io/include" APPEND)

# Genera can.so en lugar de un firmware completo.
include(elf_loader)
project_so(can)
