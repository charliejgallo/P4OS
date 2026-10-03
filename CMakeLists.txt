cmake_minimum_required(VERSION 3.16)

# P4OS firmware for the Waveshare ESP32-P4-WIFI6-Touch-LCD-5.
#
# Only what main needs is built: the components imported from AmoledOS that
# are not wired to the P4 yet (the web portal, the dynamic loader) wait in
# components/ until their phase (docs/plan/PLAN.md).
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Before project(): the fonts' definitions go into every component's flags.
include(${CMAKE_CURRENT_LIST_DIR}/components/aos_fonts/aos_fonts.cmake)

# The firmware version comes from `git describe --tags` at build time, via
# the app descriptor (aos_hal_firmware_version); not pinned here, as on the
# watch.
project(p4os)

# TinyUSB's endpoint buffers in PSRAM (components/aos_hal/tusb/aos_tusb_config.h
# says why and how): every library that compiles TinyUSB's headers takes that
# configuration file instead of esp_tinyusb's, which it includes. The managed
# components stay untouched.
idf_build_get_property(build_components BUILD_COMPONENTS)
set(AOS_TUSB_DIR ${CMAKE_CURRENT_LIST_DIR}/components/aos_hal/tusb)
foreach(c espressif__tinyusb espressif__esp_tinyusb aos_hal)
    if(c IN_LIST build_components)
        idf_component_get_property(lib ${c} COMPONENT_LIB)
        target_include_directories(${lib} PRIVATE ${AOS_TUSB_DIR})
        target_compile_definitions(${lib} PRIVATE "CFG_TUSB_CONFIG_FILE=\"aos_tusb_config.h\"")
    endif()
endforeach()
