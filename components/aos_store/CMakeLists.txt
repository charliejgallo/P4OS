# The Tienda's service (docs/plan/TIENDA.md): the catalogue, installing and
# removing packages, the receipts. No screen of its own.
idf_component_register(
    SRCS "aos_store.c"
    INCLUDE_DIRS "include"
    REQUIRES aos_hal
    PRIV_REQUIRES aos_ui aos_dynapp aos_backup json
)
