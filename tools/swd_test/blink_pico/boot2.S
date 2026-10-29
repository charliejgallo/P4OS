/*
 * The RP2040's second stage: the boot ROM copies these 256 bytes from the
 * start of the flash to SRAM, checks their CRC-32 and runs them. All this
 * one does is pico-sdk's boot2_generic_03h: the SSI in XIP mode with the
 * plain 03h read, which every QSPI flash understands, then the vector
 * table at 0x10000100. Slow (no quad mode), and enough for a blink.
 */
    .syntax unified
    .cpu cortex-m0plus
    .thumb
    .text
    .global boot2
    .thumb_func
boot2:
    ldr  r3, =0x18000000        /* XIP_SSI */
    movs r1, #0
    str  r1, [r3, #0x08]        /* SSIENR: off, to configure it */
    movs r1, #4
    str  r1, [r3, #0x14]        /* BAUDR: clk_sys / 4 */
    ldr  r1, =0x001F0300        /* CTRLR0: standard SPI, 32-bit frames, EEPROM read */
    str  r1, [r3, #0x00]
    ldr  r1, =0x03000218        /* SPI_CTRLR0: command 03h, 8-bit instruction, 24-bit address */
    ldr  r0, =0x180000F4
    str  r1, [r0]
    movs r1, #0
    str  r1, [r3, #0x04]        /* CTRLR1: one 32-bit read */
    movs r1, #1
    str  r1, [r3, #0x08]        /* SSIENR: on; the flash is mapped now */
    ldr  r0, =0x10000100        /* the program's vector table */
    ldr  r1, =0xE000ED08        /* VTOR */
    str  r0, [r1]
    ldmia r0!, {r1, r2}
    msr  msp, r1
    bx   r2
    .ltorg
