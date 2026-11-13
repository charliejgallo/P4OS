idf_component_register(
    SRCS "main.c" "xip_check.c"
    INCLUDE_DIRS "."
    REQUIRES aos_hal aos_ui aos_apps aos_io aos_portal aos_ssh aos_dynapp aos_ble
    # Nothing calls it, so with set(COMPONENTS main) it would be left out and
    # the coredump partition would never be written.
    PRIV_REQUIRES espcoredump esp_psram spi_flash esp_timer esp_mm
)

# lwIP asks P4OS before it forwards a packet from one network to another
# (components/aos_hal/include/aos_lwip_hooks.h): the way ESP-IDF's lwIP port
# takes a project's own hooks.
idf_component_get_property(lwip lwip COMPONENT_LIB)
target_compile_options(${lwip} PRIVATE "-I${PROJECT_DIR}/components/aos_hal/include")
target_compile_definitions(${lwip} PRIVATE "-DESP_IDF_LWIP_HOOK_FILENAME=\"aos_lwip_hooks.h\"")

# Diagnostic builds (xip_check.c): P4OS_XIP_CHECK=1 compares the firmware in
# PSRAM with flash at boot; P4OS_XIP_PAD=<KB> shifts a second image's layout.
# Read at configure time: `tools/build_fw.sh rev1_3 reconfigure build`.
if(DEFINED ENV{P4OS_XIP_CHECK})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE P4OS_XIP_CHECK=1)
    # sentinels around esp_hosted's DMA buffers (xip_check.c)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=eh_host_port_dma_alloc_aligned"
                                                     "-Wl,--wrap=eh_host_port_dma_free")
    if(DEFINED ENV{P4OS_XIP_PAD})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE P4OS_XIP_PAD=$ENV{P4OS_XIP_PAD})
    endif()
endif()
