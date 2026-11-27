cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(malla)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

# nanopb and the code generated from Meshtastic's .proto files live in
# main/pb (tools/gen_malla_pb.sh): project_so() compiles them with the rest,
# and their headers are found from there.
idf_build_set_property(COMPILE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/main/pb" APPEND)

# BLE pairing with a PIN needs the firmware's aos_hal_ble_gatt_pair & co.;
# with a firmware that lacks them the app still builds, and talks only to
# nodes that ask for no PIN (msh_link_ble.c).
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../components/aos_hal/include/aos_hal.h" _msh_hal)
string(FIND "${_msh_hal}" "aos_hal_ble_gatt_pair" _msh_pair)
if(_msh_pair GREATER -1)
    idf_build_set_property(COMPILE_DEFINITIONS "MSH_BLE_PAIR=1" APPEND)
endif()

# Builds malla.so instead of a whole firmware.
include(elf_loader)
project_so(malla)
