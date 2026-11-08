# P4OS SSH server: a console on the board from any ssh client (docs/SSH.md).
# The rules, the host key and the keys that may log in are aos_access's
# (components/aos_ui/aos_access_ssh.c).
idf_component_register(
    SRCS "aos_ssh.c" "aos_ssh_wire.c" "aos_ssh_crypto.c" "aos_ssh_shell.c" "aos_ssh_cmds.c"
    INCLUDE_DIRS "include"
    REQUIRES aos_hal
    PRIV_REQUIRES aos_ui monocypher mbedtls lwip
)
