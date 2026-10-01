# The serial programmer service (aos_flasher.c) and the firmware found on the
# card (aos_flasher_src.c), on top of Espressif's esp-serial-flasher, which
# comes from the component registry (idf_component.yml). HAL-clean: only
# aos_hal, aos_io and the library, so the simulator compiles the same files
# (sim/CMakeLists.txt builds the library from managed_components/).
idf_component_register(
    SRCS "aos_flasher.c" "aos_flasher_src.c"
    INCLUDE_DIRS "include"
    REQUIRES aos_hal aos_io
    PRIV_REQUIRES espressif__esp-serial-flasher
)
