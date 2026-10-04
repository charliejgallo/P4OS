cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(notas)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# Genera notas.so en lugar de un firmware completo.
include(elf_loader)
project_so(notas)
