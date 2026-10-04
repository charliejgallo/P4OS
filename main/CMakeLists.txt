idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES aos_hal aos_ui aos_apps aos_io aos_portal aos_dynapp aos_ble
    # Nothing calls it, so with set(COMPONENTS main) it would be left out and
    # the coredump partition would never be written.
    PRIV_REQUIRES espcoredump
)
