/* P4OS - the C6 as a radio co-processor for the P4's stack (rcp.c). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool     rcp_start(void);
bool     rcp_running(void);
/* spinel bytes from the P4 (peer_data's task) */
void     rcp_from_host(const uint8_t *data, size_t len);
uint32_t rcp_tx_bytes(void);
uint32_t rcp_rx_bytes(void);
/* energy measures the radio could not make (Wi-Fi refused them), since the start */
uint32_t rcp_ed_failures(void);
