# The simulator's side of Malla (included by sim/CMakeLists.txt when the app
# is in P4OS_SIM_APPS). Its glob takes main/*.c only: nanopb and the
# generated protobuf code in main/pb become a library of their own, whose
# headers every source of the simulator then sees.
file(GLOB _msh_pb_src CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_LIST_DIR}/main/pb/*.c
     ${CMAKE_CURRENT_LIST_DIR}/main/pb/meshtastic/*.c)
add_library(aos_malla_pb STATIC ${_msh_pb_src})
target_include_directories(aos_malla_pb PUBLIC ${CMAKE_CURRENT_LIST_DIR}/main/pb)
include_directories(${CMAKE_CURRENT_LIST_DIR}/main/pb)
list(APPEND AOS_SIM_APP_LIBS aos_malla_pb)

file(READ "${CMAKE_CURRENT_LIST_DIR}/../../components/aos_hal/include/aos_hal.h" _msh_hal)
string(FIND "${_msh_hal}" "aos_hal_ble_gatt_pair" _msh_pair)
if(_msh_pair GREATER -1)
    add_compile_definitions(MSH_BLE_PAIR=1)
endif()
