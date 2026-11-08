cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ssh)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# The crypto the firmware lends to the apps (tools/gen_symbols.py): SSH's
# wire types, the packet cipher and the host key check (aos_ssh), and
# Monocypher's X25519 and Ed25519. Only their headers come from here.
foreach(dir "${AOS_ROOT}/components/aos_ssh/include" "${AOS_ROOT}/components/monocypher/include")
    idf_build_set_property(COMPILE_INCLUDE_DIRECTORIES "${dir}" APPEND)
    idf_build_set_property(INCLUDE_DIRECTORIES "${dir}" APPEND)
endforeach()

# Builds ssh.so instead of a whole firmware.
include(elf_loader)
project_so(ssh)
