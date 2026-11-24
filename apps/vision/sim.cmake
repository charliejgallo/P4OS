# P4OS - Vision in the simulator (included by sim/CMakeLists.txt when the app
# is in P4OS_SIM_APPS): the engine with esp-dl's plain C kernels and the
# stand-ins for ESP-IDF (engine/compat), and quirc, which lives in a folder
# of main/ the simulator's glob does not reach.
#
# The models are the P4's: the engine copies each one and unpacks its
# convolution filters to the C kernels' layout first (VE_UNPACK_FILTERS,
# engine/ve_fbs_model.cpp). Faces, people and hands come out as on the
# board; COCO's detector finds less than there.
#
# Symbols: every app is linked into one executable, and esp-dl's names are
# generic: its objects are linked with -r into one that keeps only
# ve_module_api global, as the Emulador's cores do.
enable_language(CXX)

include(${CMAKE_CURRENT_LIST_DIR}/engine/sources.cmake)

add_library(vision_engine_sim_objs OBJECT ${VE_SRCS_COMMON} ${VE_SRCS_SIM})
target_include_directories(vision_engine_sim_objs PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/engine/compat ${CMAKE_CURRENT_LIST_DIR}/engine/gen/c ${VE_INCLUDES})
target_compile_definitions(vision_engine_sim_objs PRIVATE AOS_SIM VE_UNPACK_FILTERS=1)
target_compile_options(vision_engine_sim_objs PRIVATE -O2 -w -fno-common
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)

set(_ve_one ${CMAKE_CURRENT_BINARY_DIR}/vision_engine_sim.o)
if(APPLE)
    add_custom_command(OUTPUT ${_ve_one}
        COMMAND ${CMAKE_CXX_COMPILER} -r -nostdlib -Wl,-exported_symbol,_ve_module_api
                -o ${_ve_one} $<TARGET_OBJECTS:vision_engine_sim_objs>
        DEPENDS vision_engine_sim_objs $<TARGET_OBJECTS:vision_engine_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Vision engine into one object")
else()
    add_custom_command(OUTPUT ${_ve_one}
        COMMAND ${CMAKE_LINKER} -r -o ${_ve_one}.all $<TARGET_OBJECTS:vision_engine_sim_objs>
        COMMAND ${CMAKE_OBJCOPY} --keep-global-symbol=ve_module_api ${_ve_one}.all ${_ve_one}
        DEPENDS vision_engine_sim_objs $<TARGET_OBJECTS:vision_engine_sim_objs>
        COMMAND_EXPAND_LISTS
        COMMENT "Linking the Vision engine into one object")
endif()
set_source_files_properties(${_ve_one} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(vision_engine_sim STATIC ${_ve_one})
set_target_properties(vision_engine_sim PROPERTIES LINKER_LANGUAGE C)
if(APPLE)
    target_link_libraries(vision_engine_sim INTERFACE c++)
else()
    target_link_libraries(vision_engine_sim INTERFACE stdc++)
endif()
list(APPEND AOS_SIM_APP_LIBS vision_engine_sim)

# quirc (main/quirc), quiet: third-party
file(GLOB _vi_quirc ${CMAKE_CURRENT_LIST_DIR}/main/quirc/*.c)
add_library(vision_quirc_sim STATIC ${_vi_quirc})
target_compile_options(vision_quirc_sim PRIVATE -O2 -w)
list(APPEND AOS_SIM_APP_LIBS vision_quirc_sim)
