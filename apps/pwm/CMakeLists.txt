cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pwm)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# aos_io.h (the header's pins, PWM, the analog level): common.cmake does not
# hand it to the apps yet, so this one adds it for itself.
idf_build_set_property(INCLUDE_DIRECTORIES "${AOS_ROOT}/components/aos_io/include" APPEND)
idf_build_set_property(COMPILE_INCLUDE_DIRECTORIES "${AOS_ROOT}/components/aos_io/include" APPEND)

# Builds pwm.so instead of a whole firmware.
include(elf_loader)
project_so(pwm)
