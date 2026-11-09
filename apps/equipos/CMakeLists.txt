cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(equipos)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# The firmware's Modbus master and the Riden service (aos_modbus.h,
# aos_riden.h) live with the built-in apps, outside common.cmake's includes.
idf_build_set_property(INCLUDE_DIRECTORIES "${AOS_ROOT}/components/aos_apps/include" APPEND)
idf_build_set_property(COMPILE_INCLUDE_DIRECTORIES "${AOS_ROOT}/components/aos_apps/include" APPEND)

include(elf_loader)
project_so(equipos)
