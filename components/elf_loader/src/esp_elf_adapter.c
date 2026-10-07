/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <sys/errno.h>
#include "esp_idf_version.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "soc/soc.h"
#if CONFIG_IDF_TARGET_ESP32S31
#include "esp32s31/rom/cache.h"
#include "soc/cache_reg.h"
#endif
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp32p4/rom/cache.h"
#endif
#include "private/elf_platform.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_cache.h"
#endif

#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
#ifdef CONFIG_IDF_TARGET_ESP32S3
#define OFFSET_TEXT_VALUE   (SOC_IROM_LOW - SOC_DROM_LOW)
#endif
#endif

/**
 * @brief Allocate block of memory.
 *
 * @param n - Memory size in byte
 * @param exec - True: memory can run executable code; False: memory can R/W data
 *
 * @return Memory pointer if success or NULL if failed.
 */
void *esp_elf_malloc(uint32_t n, bool exec)
{
    uint32_t caps;

#if CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR
#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
    caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
#ifdef MALLOC_CAP_EXEC
    caps = exec ? MALLOC_CAP_EXEC : MALLOC_CAP_8BIT;
#else
    caps = MALLOC_CAP_8BIT | MALLOC_CAP_32BIT;
#endif
#endif
#else
#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
    caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    caps = MALLOC_CAP_8BIT;
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    caps |= MALLOC_CAP_CACHE_ALIGNED;
#endif
#endif

    return heap_caps_malloc(n, caps);
}

/**
 * @brief Free block of memory.
 *
 * @param ptr - memory block pointer allocated by "esp_elf_malloc"
 *
 * @return None
 */
void esp_elf_free(void *ptr)
{
    heap_caps_free(ptr);
}

/**
 * @brief Remap symbol from ".data" to ".text" section.
 *
 * @param elf  - ELF object pointer
 * @param sym  - ELF symbol table
 *
 * @return Remapped symbol value
 */
#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
uintptr_t elf_remap_text(esp_elf_t *elf, uintptr_t sym)
{
    uintptr_t mapped_sym;
    esp_elf_sec_t *sec = &elf->sec[ELF_SEC_TEXT];

    if ((sym >= sec->addr) &&
            (sym < (sec->addr + sec->size))) {
#ifdef CONFIG_ELF_LOADER_SET_MMU
        mapped_sym = sym + elf->text_off;
#else
        mapped_sym = sym + OFFSET_TEXT_VALUE;
#endif
    } else {
        mapped_sym = sym;
    }

    return mapped_sym;
}
#endif

/**
 * @brief Flush data from cache to external RAM.
 *
 * @param None
 *
 * @return None
 */
#ifdef CONFIG_ELF_LOADER_LOAD_PSRAM
void IRAM_ATTR esp_elf_arch_flush(void)
{
    extern void spi_flash_disable_interrupts_caches_and_other_cpu(void);
    extern void spi_flash_enable_interrupts_caches_and_other_cpu(void);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)

#if CONFIG_IDF_TARGET_ESP32P4
    /* P4OS: the P4's ROM takes the caches to act on (cache.h: map); the
     * generic branch below called it with none. The code was written
     * through the L1 data cache: write that back to the L2, which is shared,
     * and drop both cores' instruction caches, which may hold what was at
     * those addresses before. No MMU alias is involved: the data and the
     * instruction bus see PSRAM at the same address on this chip. */
    Cache_WriteBack_All(CACHE_MAP_L1_DCACHE);
    Cache_Invalidate_All(CACHE_MAP_L1_ICACHE_MASK);
#elif CONFIG_IDF_TARGET_ESP32S31
    /* ESP32-S31: Ranged cache APIs (Cache_WriteBack_Addr/Cache_Invalidate_Addr)
     * cause intermittent Instruction access faults (MCAUSE=0x01,
     * MEPC=0x00000000) during long-term ELF execution from PSRAM, likely
     * related to unaligned addr/size (e.g. seg_size=0x136a8, cache_line=64B).
     * Use full D-writeback + I-invalidate like other targets. */
    Cache_WriteBack_All(CACHE_MAP_L1_DCACHE);
    spi_flash_disable_interrupts_caches_and_other_cpu();
    Cache_Invalidate_All(CACHE_MAP_L1_ICACHE_MASK);
    spi_flash_enable_interrupts_caches_and_other_cpu();
    REG_CLR_BIT(CACHE_L1_ICACHE_CTRL_REG, CACHE_L1_ICACHE_SHUT_IBUS1);
#else
    extern void Cache_WriteBack_All(void);
    Cache_WriteBack_All();
    spi_flash_disable_interrupts_caches_and_other_cpu();
    spi_flash_enable_interrupts_caches_and_other_cpu();
#endif
#else
    void esp_spiram_writeback_cache(void);

    esp_spiram_writeback_cache();
    spi_flash_disable_interrupts_caches_and_other_cpu();
    spi_flash_enable_interrupts_caches_and_other_cpu();
#endif
}

#if CONFIG_IDF_TARGET_ESP32P4
/* P4OS: the loaded code's own lines only, through IDF's ranged cache API.
 * esp_elf_arch_flush() writes back the whole L1 data cache and drops both
 * cores' whole instruction caches with the ROM's *_All calls while the other
 * core runs (LVGL, the boot screen), which IDF itself never does; the boot
 * crashes of 2026-10-04/06 (a module's symbol table read back broken right
 * after dlopen, the PSRAM heap's free lists broken under LVGL, all during the
 * card's app scan) pointed there. The code blocks are 128-byte aligned and
 * sized (__wrap_esp_elf_malloc in aos_dynapp.c), the L2 line, so syncing the
 * whole block never touches anyone else's data. */
void esp_elf_arch_flush_code(const void *code)
{
    size_t n = code ? heap_caps_get_allocated_size((void *)code) & ~(size_t)127 : 0;
    if (!n || ((uintptr_t)code & 127)) {
        esp_elf_arch_flush();
        return;
    }
    esp_cache_msync((void *)code, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    esp_cache_msync((void *)code, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
}
#endif
#endif
