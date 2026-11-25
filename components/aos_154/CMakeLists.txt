# The 802.15.4 radio (Zigbee, Thread), the C6's, lent to the P4 over
# esp_hosted's peer_data channel (docs/plan/ZIGBEE.md). aos_154.c is the
# simulator's too; the link is sim/aos_154_link_sim.c there.
# aos_zigbee_p4.c: Espressif's Zigbee stack here with the C6 as its RCP.
idf_component_register(SRCS "aos_154.c" "aos_zigbee_fmt.c" "aos_zigbee_ir.c" "aos_zigbee_tuya.c" "aos_154_link_p4.c" "aos_zigbee_p4.c" "aos_zigbee_taskq.c" "aos_zigbee_trace.c"
                             "aos_zigbee_devices.c" "aos_zigbee_emu.c" "aos_zigbee_nvs.c"
                       INCLUDE_DIRS "include"
                       REQUIRES aos_hal
                       PRIV_REQUIRES espressif__esp_hosted espressif__esp-zigbee-lib openthread lwip vfs aos_ui mbedtls nvs_flash
                       # whole: nothing of ours calls the __wrap_ functions of
                       # aos_zigbee_taskq.c, only Espressif's libraries do
                       WHOLE_ARCHIVE)

# the stack's link to its RCP is a loopback socket to the C6's, not a UART
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_zigbee_radio_spinel_config_uart")

# while emulating a device, the stack's network in its own NVS namespace
# (aos_zigbee_nvs.c)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=nvs_open_from_partition")

# esp-zigbee-lib's task queue, again, for a P4 before rev 3 (aos_zigbee_taskq.c)
foreach(fn esp_zigbee_task_queue_init esp_zigbee_task_queue_deinit esp_zigbee_task_queue_post
           esp_zigbee_task_queue_update esp_zigbee_task_queue_process ezb_tasklet_signal_pending
           ezb_plat_log)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${fn}")
endforeach()

# while measuring: counters on the stack's platform calls (aos_zigbee_trace.c)
foreach(fn ezb_plat_milli_alarm_start_at ezb_plat_micro_alarm_start_at ezb_plat_signal_milli_alarm_fired
           ezb_plat_signal_micro_alarm_fired ezb_plat_radio_enable ezb_plat_radio_receive ezb_plat_radio_transmit
           ezb_plat_radio_energy_detect ezb_plat_radio_energy_detect_done ezb_plat_radio_receive_done
           ezb_plat_radio_transmit_done)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${fn}")
endforeach()
