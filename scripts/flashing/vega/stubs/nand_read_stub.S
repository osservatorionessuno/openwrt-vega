/*
 * nand_read_stub.S — SPI-NAND page reader for Nuclei UX600/UX608.
 *
 * Run from ILM (0x80000000) under JTAG control. The CPU is halted, the stub
 * is loaded, argument registers are set, and the CPU is resumed. The stub
 * fills (count * 2048) bytes at dst, then ebreaks back to JTAG.
 *
 * Inputs (set via JTAG before resume):
 *   a0 = SPI controller base   (0x10016000 for the SPI-NAND on this SoC)
 *   a1 = destination buffer    (DRAM, must hold count * 2048 bytes)
 *   a2 = first page index      (0 .. 65535 for a 1 Gbit chip)
 *   a3 = page count
 *
 * Build:
 *   ${CROSS}-gcc -nostdlib -nostartfiles -march=rv64imafdc -mabi=lp64 \
 *       -Wl,-Ttext=0x80000000 -o nand_read_stub.elf nand_read_stub.S
 *   ${CROSS}-objcopy -O binary nand_read_stub.elf nand_read_stub.bin
 *
 * SPI register offsets (SiFive QSPI / NUSPI IP):
 *   +0x18 CSMODE   (0 = auto, 2 = HOLD)
 *   +0x40 FMT      (bit 3 = direction; 1 = TX-only, 0 = RX-only)
 *   +0x48 TXDATA   (write to push; bit 31 read = FIFO full)
 *   +0x4c RXDATA   (read pops; bit 31 set = empty)
 *   +0x50 TXMARK   (TX watermark)
 *   +0x74 IP       (bit 0 = tx_below_mark)
 *
 * SPI-NAND command set (GigaDevice GD5F1GQ4 family):
 *   0x13 PAGE_READ          + 3-byte row addr  : load page to cache
 *   0x0F GET_FEATURES + reg                    : read a feature register
 *        reg 0xC0 = STATUS  (bit 0 = OIP, operation in progress)
 *   0x0B READ_FROM_CACHE  + 2-byte col + dummy : stream cache bytes out
 */

    .text
    .globl _start
_start:

page_loop:
    beqz    a3, done

    /* ---- 1) PAGE_READ (0x13) + 3-byte page address ---- */
    li      t0, 2
    sw      t0, 0x18(a0)            /* CSMODE = HOLD               */
    li      t0, 1
    sw      t0, 0x50(a0)            /* TXMARK = 1                  */
    li      t0, 0x80008
    sw      t0, 0x40(a0)            /* FMT len=8, dir=TX           */

    li      t0, 0x13
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    srli    t0, a2, 16
    andi    t0, t0, 0xff
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    srli    t0, a2, 8
    andi    t0, t0, 0xff
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    andi    t0, a2, 0xff
    sw      t0, 0x48(a0)
    jal     ra, wait_tx

    sw      zero, 0x18(a0)          /* CS off                      */

    /* ---- 2) Poll OIP via GET_FEATURES 0xC0 ---- */
poll_oip:
    li      t0, 2
    sw      t0, 0x18(a0)            /* CSMODE = HOLD               */
    li      t0, 0x80008
    sw      t0, 0x40(a0)            /* FMT len=8, dir=TX           */

    li      t0, 0x0F
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    li      t0, 0xC0
    sw      t0, 0x48(a0)
    jal     ra, wait_tx

    /* drain any RX leftovers */
1:  lw      t0, 0x4c(a0)
    bgez    t0, 1b                  /* loop while bit31 = 0 (not empty) */

    li      t0, 0x80000
    sw      t0, 0x40(a0)            /* FMT len=8, dir=RX           */
    sw      zero, 0x48(a0)          /* dummy TX                    */
    jal     ra, wait_tx
2:  lw      t0, 0x4c(a0)
    bltz    t0, 2b                  /* loop while empty            */
    sw      zero, 0x18(a0)          /* CS off                      */
    andi    t0, t0, 1
    bnez    t0, poll_oip            /* OIP still set, poll again   */

    /* ---- 3) READ_FROM_CACHE (0x0B) + col(2 bytes) + dummy ---- */
    li      t0, 2
    sw      t0, 0x18(a0)            /* CS HOLD                     */
    li      t0, 0x80008
    sw      t0, 0x40(a0)            /* FMT len=8, dir=TX           */

    li      t0, 0x0B
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    sw      zero, 0x48(a0)          /* col[15:8] = 0               */
    jal     ra, wait_tx
    sw      zero, 0x48(a0)          /* col[7:0] = 0                */
    jal     ra, wait_tx
    sw      zero, 0x48(a0)          /* dummy                       */
    jal     ra, wait_tx

    /* drain RX */
3:  lw      t0, 0x4c(a0)
    bgez    t0, 3b

    li      t0, 0x80000
    sw      t0, 0x40(a0)            /* FMT len=8, dir=RX           */

    li      t1, 2048                /* bytes left in page          */
    mv      t2, a1                  /* dst pointer                 */
read_byte_loop:
    sw      zero, 0x48(a0)          /* dummy TX                    */
4:  lw      t0, 0x74(a0)
    andi    t0, t0, 1
    beqz    t0, 4b                  /* wait for tx-below-mark      */
5:  lw      t0, 0x4c(a0)
    bltz    t0, 5b                  /* wait until RX has byte      */
    sb      t0, 0(t2)
    addi    t2, t2, 1
    addi    t1, t1, -1
    bnez    t1, read_byte_loop

    sw      zero, 0x18(a0)          /* CS off                      */

    /* ---- 4) advance ---- */
    addi    a1, a1, 2048
    addi    a2, a2, 1
    addi    a3, a3, -1
    j       page_loop

done:
    ebreak
    j       done                    /* in case JTAG misses the ebreak */

wait_tx:
    lw      t0, 0x74(a0)
    andi    t0, t0, 1
    beqz    t0, wait_tx
    ret
