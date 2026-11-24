# The 802.15.4 radio (Zigbee, Thread), the C6's, lent to the P4 over
# esp_hosted's peer_data channel (docs/plan/ZIGBEE.md). aos_154.c is the
# simulator's too; the link is sim/aos_154_link_sim.c there.
idf_component_register(SRCS "aos_154.c" "aos_154_link_p4.c"
                       INCLUDE_DIRS "include"
                       REQUIRES aos_hal
                       PRIV_REQUIRES espressif__esp_hosted)
