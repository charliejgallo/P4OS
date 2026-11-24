# P4OS - Vision: the engine's sources, for the board (board.cmake) and the
# simulator (../sim.cmake). Sets:
#   VE_SRCS_COMMON   esp-dl's C++ and ours, the same on both
#   VE_SRCS_BOARD    the P4's SIMD kernels (assembly) and the runtime pieces
#   VE_SRCS_SIM      esp-dl's plain C kernels
#   VE_INCLUDES      esp-dl's include directories (the generated files and
#                    the simulator's stand-ins are added by each side)
#   VE_INCLUDES_BOARD  and the P4's assembly headers
set(VE_DIR ${CMAKE_CURRENT_LIST_DIR})
set(VE_DL ${VE_DIR}/espdl)

file(GLOB VE_SRCS_COMMON CONFIGURE_DEPENDS
    ${VE_DL}/dl/tool/src/*.cpp
    ${VE_DL}/dl/tensor/src/*.cpp
    ${VE_DL}/dl/math/src/*.cpp
    ${VE_DL}/dl/model/src/*.cpp
    ${VE_DL}/dl/module/src/*.cpp
    ${VE_DL}/dl/base/*.cpp
    ${VE_DL}/vision/detect/*.cpp
    ${VE_DL}/vision/image/*.cpp
    ${VE_DL}/vision/classification/*.cpp)
list(APPEND VE_SRCS_COMMON
    ${VE_DIR}/ve_engine.cpp
    ${VE_DIR}/ve_fbs_model.cpp
    ${VE_DIR}/ve_fbs_loader.cpp)

file(GLOB VE_SRCS_BOARD CONFIGURE_DEPENDS
    ${VE_DL}/dl/base/isa/esp32p4/*.S
    ${VE_DL}/dl/tool/isa/esp32p4/*.S
    ${VE_DL}/vision/image/isa/esp32p4/*.S)
list(APPEND VE_SRCS_BOARD ${VE_DIR}/ve_rt.cpp ${VE_DIR}/rt/tree.cc ${VE_DIR}/rt/list.cc
    ${VE_DIR}/rt/hashtable_c++0x.cc ${VE_DIR}/rt/hash_bytes.cc)

file(GLOB VE_SRCS_SIM CONFIGURE_DEPENDS ${VE_DL}/dl/base/isa/c/*.cpp)

set(VE_INCLUDES ${VE_DIR} ${VE_DIR}/../main)
foreach(_d dl dl/tool/include dl/tensor/include dl/base dl/base/isa dl/base/isa/c dl/math/include
        dl/model/include dl/module/include fbs_loader/include vision/detect vision/image vision/image/isa
        vision/recognition vision/classification)
    list(APPEND VE_INCLUDES ${VE_DL}/${_d})
endforeach()
set(VE_INCLUDES_BOARD ${VE_DL}/dl/base/isa/esp32p4 ${VE_DL}/dl/base/isa/riscv)
