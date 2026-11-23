cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(plotter)

# common.cmake brings aos_io.h too (the serial capture service, aos_serial_*).
include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# Genera plotter.so en lugar de un firmware completo.
include(elf_loader)
project_so(plotter)
