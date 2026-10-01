/*
 * P4OS - TinyUSB's configuration: esp_tinyusb's, with the endpoint buffers
 * in PSRAM (internal RAM audit, docs/MEMORY.md).
 *
 * The project's CMakeLists.txt points TinyUSB at this file
 * (CFG_TUSB_CONFIG_FILE) for the libraries that compile its headers:
 * TinyUSB, esp_tinyusb and aos_hal. The components stay untouched.
 *
 * esp_tinyusb puts the buffers the USB DMA reads and writes (the setup
 * packet, and each class's endpoint buffers: MSC, NCM, HID, MIDI) in
 * internal RAM, 21 KB of it with MSC and NCM. On the P4 the USB DMA reaches
 * PSRAM as well (IDF says so for its own USB host,
 * USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM), and TinyUSB's DWC2 port already
 * cleans and invalidates the data cache around every transfer. What changes
 * is the cache line: PSRAM sits behind the L2 cache, whose line is 128
 * bytes, and esp_cache_msync() refuses anything not aligned to it. So the
 * buffers are aligned and padded to 128, and the rounding dwc2_esp32.h does
 * before each cache operation (to CONFIG_CACHE_L1_CACHE_LINE_SIZE) is made
 * 128 too, inside these libraries only: nothing else here uses that value.
 */
#pragma once

#include "tusb_config.h"        /* esp_tinyusb's own; it brings sdkconfig.h */
#include "esp_attr.h"

#if CFG_TUD_MEM_DCACHE_ENABLE && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY

#define AOS_TUSB_LINE 128       /* the L2 cache's, PSRAM's */

#undef  CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION __attribute__((aligned(AOS_TUSB_LINE))) EXT_RAM_BSS_ATTR

#undef  CFG_TUD_MEM_DCACHE_LINE_SIZE
#define CFG_TUD_MEM_DCACHE_LINE_SIZE AOS_TUSB_LINE
/* the host side's default follows this one, and dwc2_esp32.h checks that
 * both match the line it rounds to */
#undef  CFG_TUSB_MEM_DCACHE_LINE_SIZE
#define CFG_TUSB_MEM_DCACHE_LINE_SIZE AOS_TUSB_LINE
#undef  CFG_TUH_MEM_DCACHE_LINE_SIZE
#define CFG_TUH_MEM_DCACHE_LINE_SIZE AOS_TUSB_LINE

/* dwc2_esp32.h: round_up_to_cache_line_size() */
#undef  CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define CONFIG_CACHE_L1_CACHE_LINE_SIZE AOS_TUSB_LINE

#endif
