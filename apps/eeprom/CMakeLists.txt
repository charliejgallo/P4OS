cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(eeprom)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# La primera app de tarjeta que habla con el conector de atras: common.cmake
# no trae los headers de aos_io (puertos, I2C, SPI, GPIO), asi que se suman
# aca, para el componente y para el paso de project_so().
idf_build_set_property(INCLUDE_DIRECTORIES "${AOS_ROOT}/components/aos_io/include" APPEND)
idf_build_set_property(COMPILE_INCLUDE_DIRECTORIES "${AOS_ROOT}/components/aos_io/include" APPEND)

# Genera eeprom.so en lugar de un firmware completo.
include(elf_loader)
project_so(eeprom)
