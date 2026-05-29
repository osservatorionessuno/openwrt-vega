/*
 * nand_write_stub.S — SPI-NAND block-erase + page-program for Nuclei UX600/UX608.
 *
 * Inputs (set via JTAG before resume):
 *   a0 = SPI controller base       (0x10016000)
 *   a1 = source buffer (DRAM)       — holds count * 2048 bytes
 *   a2 = first page index           (must be block-aligned for first page,
 *                                    i.e. page % 64 == 0; subsequent pages
 *                                    just increment)
 *   a3 = page count                 (caller must ensure count is a multiple
 *                                    of 64 unless they only want to write
 *                                    pages 0..count-1 of an already-erased
 *                                    region)
 *   a4 = flags  (bit 0 = erase blocks before writing)
 *
 * The stub:
 *   1. If flags.erase: for every block touched by [page .. page+count),
 *      issues WRITE_ENABLE (0x06) + BLOCK_ERASE (0xD8 + 3-byte page-of-block)
 *      and polls OIP until done.
 *   2. For each page, issues WRITE_ENABLE + PROGRAM_LOAD (0x02 + 2-byte col
 *      + 2048 data) + PROGRAM_EXECUTE (0x10 + 3-byte row) and polls OIP.
 *   3. ebreaks when done. On a program/erase failure, the chip will set
 *      the E_FAIL / P_FAIL bits in STATUS but this stub does not abort —
 *      caller should re-read STATUS via the read stub if needed.
 *
 * Build identically to nand_read_stub.S.
 */

    .text
    .globl _start
_start:

    /* ---- optional erase phase ---- */
    andi    t3, a4, 1
    beqz    t3, prog_phase

    /* compute first block = a2 / 64 ; last block exclusive */
    srli    s0, a2, 6                   /* first block            */
    add     s1, a2, a3
    addi    s1, s1, 63
    srli    s1, s1, 6                   /* last block exclusive   */
erase_loop:
    beq     s0, s1, erase_done
    /* WRITE_ENABLE (0x06) */
    li      t0, 2
    sw      t0, 0x18(a0)
    li      t0, 1
    sw      t0, 0x50(a0)
    li      t0, 0x80008
    sw      t0, 0x40(a0)
    li      t0, 0x06
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    sw      zero, 0x18(a0)

    /* BLOCK_ERASE (0xD8) + 3-byte page-of-block (block*64) */
    li      t0, 2; sw t0, 0x18(a0)
    li      t0, 0x80008; sw t0, 0x40(a0)
    li      t0, 0xD8
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    slli    t4, s0, 6                   /* page-of-block          */
    srli    t0, t4, 16; andi t0, t0, 0xff
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    srli    t0, t4, 8; andi t0, t0, 0xff
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    andi    t0, t4, 0xff
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    sw      zero, 0x18(a0)

    jal     ra, wait_oip
    addi    s0, s0, 1
    j       erase_loop
erase_done:

    /* ---- program phase ---- */
prog_phase:
    mv      s0, a1                       /* src ptr                */
    mv      s1, a2                       /* page index             */
    mv      s2, a3                       /* pages left             */

page_loop:
    beqz    s2, all_done

    /* WRITE_ENABLE */
    li      t0, 2; sw t0, 0x18(a0)
    li      t0, 1; sw t0, 0x50(a0)
    li      t0, 0x80008; sw t0, 0x40(a0)
    li      t0, 0x06
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    sw      zero, 0x18(a0)

    /* PROGRAM_LOAD (0x02) + col[15:8] + col[7:0] + 2048 data */
    li      t0, 2; sw t0, 0x18(a0)
    li      t0, 0x80008; sw t0, 0x40(a0)
    li      t0, 0x02
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    sw      zero, 0x48(a0)               /* col[15:8] = 0          */
    jal     ra, wait_tx
    sw      zero, 0x48(a0)               /* col[7:0]  = 0          */
    jal     ra, wait_tx

    li      t1, 2048
    mv      t2, s0
prog_byte_loop:
    lbu     t0, 0(t2)
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    addi    t2, t2, 1
    addi    t1, t1, -1
    bnez    t1, prog_byte_loop
    sw      zero, 0x18(a0)

    /* PROGRAM_EXECUTE (0x10) + 3-byte row */
    li      t0, 2; sw t0, 0x18(a0)
    li      t0, 0x80008; sw t0, 0x40(a0)
    li      t0, 0x10
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    srli    t0, s1, 16; andi t0, t0, 0xff
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    srli    t0, s1, 8;  andi t0, t0, 0xff
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    andi    t0, s1, 0xff
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    sw      zero, 0x18(a0)

    jal     ra, wait_oip

    addi    s0, s0, 2048
    addi    s1, s1, 1
    addi    s2, s2, -1
    j       page_loop

all_done:
    ebreak
    j       all_done

/* poll OIP=0; assumes caller already deasserted CS.
 * wait_tx is called internally and would clobber ra; save it in s11. */
wait_oip:
    mv      s11, ra
1:  li      t0, 2; sw t0, 0x18(a0)
    li      t0, 0x80008; sw t0, 0x40(a0)
    li      t0, 0x0F
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
    li      t0, 0xC0
    sw      t0, 0x48(a0)
    jal     ra, wait_tx
2:  lw      t0, 0x4c(a0); bgez t0, 2b
    li      t0, 0x80000; sw t0, 0x40(a0)
    sw      zero, 0x48(a0)
    jal     ra, wait_tx
3:  lw      t0, 0x4c(a0); bltz t0, 3b
    sw      zero, 0x18(a0)
    andi    t0, t0, 1
    bnez    t0, 1b
    mv      ra, s11
    ret

wait_tx:
    lw      t0, 0x74(a0)
    andi    t0, t0, 1
    beqz    t0, wait_tx
    ret
