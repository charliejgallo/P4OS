# Backups (docs/plan/RESPALDO.md): the preferences, the list of apps and the
# small files of the card in one file, and back. No screen of its own.
idf_component_register(
    SRCS "aos_backup.c"
    INCLUDE_DIRS "include"
    REQUIRES aos_hal
    PRIV_REQUIRES json mbedtls aos_ssh
)
