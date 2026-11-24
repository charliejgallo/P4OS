/* P4OS - the C6's 802.15.4 radio, lent to the P4 (radio154.c). */
#pragma once

/* Registers with esp_hosted's peer_data and starts the task; the radio stays
 * off until the P4 asks for it. */
void radio154_init(void);
