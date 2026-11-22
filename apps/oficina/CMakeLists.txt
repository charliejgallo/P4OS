cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(oficina)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# Genera oficina.so en lugar de un firmware completo.
include(elf_loader)
project_so(oficina)
