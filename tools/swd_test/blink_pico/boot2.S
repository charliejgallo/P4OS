/*
 * The RP2040's second stage: the boot ROM copies these 256 bytes from the
 * start of the flash to SRAM, checks their CRC-32 and runs them. All this
 * one does is pico-sdk's boot2_generic_03h: the SSI in XIP mode with the
 * plain 03h read, which every QSPI flash understands, then the vector
 * table at 0x10000100. Slow (no quad mode), and enough for a blink.
 *
 * Built with -DQUAD it does what pico-sdk's boot2_w25q080 does for a
 * Pico's Winbond flash instead: the QE bit set in status register 2 if it
 * was not, then XIP with the quad EBh read in continuous mode, the clock
 * at clk_sys / 2 and the sample delayed one cycle, so it keeps up once
 * the program takes clk_sys to 125 MHz (62.5 MHz on the flash).
 */
    .syntax unified
    .cpu cortex-m0plus
    .thumb
    .text
    .global boot2
    .thumb_func
boot2:
#ifdef QUAD
    ldr  r3, =0x40020000        /* PADS_QSPI */
    movs r0, #0x21              /* SCLK: 8 mA, fast slew */
    str  r0, [r3, #0x04]
    ldr  r0, [r3, #0x08]        /* SD0..SD3: no Schmitt trigger */
    movs r1, #2
    bics r0, r1
    str  r0, [r3, #0x08]
    str  r0, [r3, #0x0C]
    str  r0, [r3, #0x10]
    str  r0, [r3, #0x14]

    ldr  r3, =0x18000000        /* XIP_SSI */
    movs r1, #0
    str  r1, [r3, #0x08]        /* SSIENR: off, to configure it */
    movs r1, #2
    str  r1, [r3, #0x14]        /* BAUDR: clk_sys / 2 */
    movs r1, #1
    movs r2, #0xF0
    str  r1, [r3, r2]           /* RX_SAMPLE_DLY: one cycle */
    ldr  r1, =0x00070000        /* CTRLR0: standard SPI, 8-bit frames, TX and RX */
    str  r1, [r3, #0x00]
    movs r1, #1
    str  r1, [r3, #0x08]

    movs r0, #0x35              /* status register 2: QE already set? */
    bl   sreg
    movs r2, #0x02
    cmp  r0, r2
    beq  qe_set
    movs r1, #0x06              /* write enable */
    str  r1, [r3, #0x60]
    bl   idle
    ldr  r1, [r3, #0x60]
    movs r1, #0x01              /* write status: SR1 0, SR2 QE */
    str  r1, [r3, #0x60]
    movs r0, #0
    str  r0, [r3, #0x60]
    str  r2, [r3, #0x60]
    bl   idle
    ldr  r1, [r3, #0x60]
    ldr  r1, [r3, #0x60]
    ldr  r1, [r3, #0x60]
1:  movs r0, #0x05              /* until the write is over */
    bl   sreg
    movs r1, #1
    tst  r0, r1
    bne  1b
qe_set:
    movs r1, #0
    str  r1, [r3, #0x08]
    ldr  r1, =0x005F0300        /* CTRLR0: quad, 32-bit frames, EEPROM read */
    str  r1, [r3, #0x00]
    movs r1, #0
    str  r1, [r3, #0x04]        /* CTRLR1: one read */
    ldr  r0, =0x180000F4        /* SPI_CTRLR0 */
    ldr  r1, =0x00002221        /* 8-bit EBh in standard SPI, 32 bits of address and mode in quad, 4 wait cycles */
    str  r1, [r0]
    movs r1, #1
    str  r1, [r3, #0x08]
    movs r1, #0xEB              /* one read with mode bits A0h, which leaves the flash in continuous mode */
    str  r1, [r3, #0x60]
    movs r1, #0xA0
    str  r1, [r3, #0x60]
    bl   idle
    movs r1, #0
    str  r1, [r3, #0x08]
    ldr  r1, =0xA0002022        /* from now on no command: address and A0h in quad, 4 wait cycles */
    str  r1, [r0]
    movs r1, #1
    str  r1, [r3, #0x08]        /* the flash is mapped now */
#else
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
#endif
    ldr  r0, =0x10000100        /* the program's vector table */
    ldr  r1, =0xE000ED08        /* VTOR */
    str  r0, [r1]
    ldmia r0!, {r1, r2}
    msr  msp, r1
    bx   r2

#ifdef QUAD
/* A status register: the command in r0, the value back in r0 */
sreg:
    push {r1, lr}
    str  r0, [r3, #0x60]
    str  r0, [r3, #0x60]        /* a byte more, for the answer */
    bl   idle
    ldr  r0, [r3, #0x60]
    ldr  r0, [r3, #0x60]
    pop  {r1, pc}

/* Until the SSI sent everything: TX FIFO empty and not busy */
idle:
    push {r0, r1, lr}
1:  ldr  r1, [r3, #0x28]        /* SR */
    movs r0, #4
    tst  r1, r0
    beq  1b
    movs r0, #1
    tst  r1, r0
    bne  1b
    pop  {r0, r1, pc}
#endif
    .ltorg
