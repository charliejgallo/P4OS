# P4OS - Vision: the engine for the board (included by ../CMakeLists.txt):
# esp-dl with the P4's SIMD kernels and our pieces, as the static library
# ve_engine, which ../CMakeLists.txt links into the module engine.so.
#
# Position-independent (it ends up inside a shared object), sections per
# function (the module's --gc-sections drops what nothing calls: esp-dl
# brings every operator), hidden, no exceptions, no RTTI, no thread-safe
# statics. esp-dl is third-party and quiet (-w); its own CMake builds it -O3
# with -ffast-math, which is kept for the C++ (the kernels that matter are
# the assembly). After the archive is made, its objects are linked with -r
# into one where ve_module_api is the only global and every COMDAT group is
# renamed (the Emulador's one_object.cmake, for the same reasons).
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

get_filename_component(_ve_repo "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
idf_build_get_property(_ve_idf IDF_PATH)

add_library(ve_engine STATIC ${VE_SRCS_COMMON} ${VE_SRCS_BOARD})
target_include_directories(ve_engine PRIVATE ${CMAKE_CURRENT_LIST_DIR}/gen/p4 ${VE_INCLUDES} ${VE_INCLUDES_BOARD})
target_compile_definitions(ve_engine PRIVATE VE_SINGLE_CORE=1 NDEBUG)
target_compile_options(ve_engine PRIVATE
    -fPIC
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden
    -ffile-prefix-map=${_ve_repo}/=
    -ffile-prefix-map=${_ve_idf}/=esp-idf/
    -w
    $<$<COMPILE_LANGUAGE:CXX>:-O3>
    $<$<COMPILE_LANGUAGE:CXX>:-ffast-math>
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
# the headers esp-dl and ve_rt.cpp take from ESP-IDF (log, heap, mmu, timer...)
idf_build_get_property(_ve_incs COMPILE_INCLUDE_DIRECTORIES)
target_include_directories(ve_engine PRIVATE ${_ve_incs})
set_target_properties(ve_engine PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/engine)

set(_ve_one ${CMAKE_BINARY_DIR}/engine/ve_engine_one.o)
set(_ve_objcopy ${CMAKE_OBJCOPY})
if(NOT _ve_objcopy)
    string(REGEX REPLACE "gcc$" "objcopy" _ve_objcopy ${CMAKE_C_COMPILER})
endif()
string(REGEX REPLACE "objcopy$" "readelf" _ve_readelf ${_ve_objcopy})
add_custom_command(TARGET ve_engine POST_BUILD
    COMMAND ${CMAKE_C_COMPILER} -nostdlib -r -o ${_ve_one}.all "$<TARGET_OBJECTS:ve_engine>"
    COMMAND ${CMAKE_COMMAND} -DIN=${_ve_one}.all -DOUT=${_ve_one} -DKEEP=ve_module_api
            -DTAG=vision -DREADELF=${_ve_readelf} -DOBJCOPY=${_ve_objcopy}
            -P ${CMAKE_CURRENT_LIST_DIR}/../../emulador/cores/a2600/one_object.cmake
    COMMAND ${CMAKE_COMMAND} -E remove $<TARGET_FILE:ve_engine>
    COMMAND ${CMAKE_AR} qc $<TARGET_FILE:ve_engine> ${_ve_one}
    COMMAND ${CMAKE_RANLIB} $<TARGET_FILE:ve_engine>
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Leaving ve_module_api the only global of the Vision engine")
