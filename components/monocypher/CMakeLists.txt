# Monocypher 4.0.2 (Loup Vaillant and others, BSD-2-Clause or CC0-1.0),
# vendored unchanged from monocypher.org: src/monocypher.c and
# src/optional/monocypher-ed25519.c with their headers. P4OS uses X25519 and
# Ed25519 from it for the SSH server (components/aos_ssh) and the SSH host
# key in Settings (components/aos_ui/aos_access_ssh.c). mbedTLS has neither
# Ed25519 nor an easy X25519.
idf_component_register(
    SRCS "monocypher.c" "monocypher-ed25519.c"
    INCLUDE_DIRS "include"
)
