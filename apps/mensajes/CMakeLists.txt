cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mensajes)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# Monocypher's SHA-512 and HMAC (PBKDF2 for the two-step password) come from
# the firmware (tools/gen_symbols.py lends them); only the header is needed.
foreach(dir "${AOS_ROOT}/components/monocypher/include")
    idf_build_set_property(COMPILE_INCLUDE_DIRECTORIES "${dir}" APPEND)
    idf_build_set_property(INCLUDE_DIRECTORIES "${dir}" APPEND)
endforeach()

# Builds mensajes.so instead of a whole firmware.
include(elf_loader)
project_so(mensajes)
