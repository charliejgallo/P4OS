# SPDX-License-Identifier: Apache-2.0
#
# The ESP32-C6's firmware: esp_hosted's co-processor side, Wi-Fi and BLE
# over SDIO to the P4. Espressif's examples/bluetooth/esp_hosted_nimble/
# bleprph_wifi_coex/cp, pinned to the same esp_hosted as the P4. Built by
# tools/build_c6.sh; the P4 installs it into the C6 (docs/C6.md).
cmake_minimum_required(VERSION 3.5)
# the version the P4 shows for an image (Settings, Update): esp_hosted's, which
# is what matters to the P4, not this repo's git describe; after the dash,
# P4OS's own additions (154.1: the 802.15.4 radio, AOS_154_PROTO 1; 154.2:
# its free memory in the status, AOS_154_PROTO 2; 154.3: Espressif's Zigbee
# stack on request, AOS_154_PROTO 3; 154.4: grows its own app slots on
# request, AOS_154_PROTO 4; 154.5: an RCP for a stack on the P4, spinel over
# peer_data, AOS_154_PROTO 5)
set(PROJECT_VER "3.0.9-154.15")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4os_c6)
