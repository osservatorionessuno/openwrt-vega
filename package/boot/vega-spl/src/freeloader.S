/*
 * Milk-V Vega first-stage SPL. Brings up DDR, enables caches,
 * copies OpenSBI/U-Boot/DTB from NOR to DDR, jumps into OpenSBI.
 *
 * Copyright (C) 2020 Ruigang Wan <rgwan@nucleisys.com>
 * Copyright (C) 2020 Nuclei System Technologies
 */

#define OPENSBI_START_BASE	0x41000000
#define UBOOT_START_BASE	0x41200000
#define FDT_START_BASE		0x48000000
#define COPY_START_BASE		0x48100000

#define CSR_MCACHE_CTL      0x7CA
#define CSR_CACHE_ENABLE    0x10001	/* bit 0 = IC_EN, bit 16 = DC_EN */

/* struct fw_dynamic_info, OpenSBI 1.6 (sbi/fw_dynamic.h) */
#define FW_DYNAMIC_INFO_MAGIC_VALUE	0x4942534f	/* 'OSBI' */
#define FW_DYNAMIC_INFO_VERSION_2	0x2
#define FW_DYNAMIC_INFO_NEXT_MODE_S	0x1

.global _start
.section .text
_start:
	/* Disable I/D cache */
	li t0, CSR_CACHE_ENABLE
	csrc CSR_MCACHE_CTL, t0

	/* Exception entry */
	lla t0, exc_entry
	csrw mtvec, t0

	/* DDR controller init */
	li	a5,268537856
	sw	zero,20(a5)
	li	a4,268537856
.L2:
	lw	a5,20(a4)
	andi	a5,a5,2
	beqz	a5,.L2
	li	a5,134258688
	addiw	a5,a5,-765
	sw	a5,512(a4)
	li	a5,270336
	sw	zero,516(a4)
	addiw	a5,a5,-912
	sw	a5,520(a4)
	li	a5,24
	sw	a5,524(a4)
	li	a5,-2147483648
	addiw	a5,a5,84
	sw	a5,528(a4)
	li	a5,520556544
	addiw	a5,a5,-1529
	sw	a5,532(a4)
	li	a5,289738752
	li	a2,8192
	addiw	a5,a5,530
	sw	a5,536(a4)
	addiw	a3,a2,48
	li	a5,28672
	sw	a3,540(a4)
	addiw	a3,a5,-254
	sw	a3,544(a4)
	li	a3,40960
	addiw	a3,a3,-1639
	li	a1,4096
	sw	a3,548(a4)
	addiw	a1,a1,-839
	sw	a1,552(a4)
	li	a1,-1879048192
	sw	zero,556(a4)
	addiw	a1,a1,4
	sw	a1,560(a4)
	li	a1,50528256
	addiw	a1,a1,771
	sw	a1,564(a4)
	sw	a1,568(a4)
	li	a1,262144
	addiw	a1,a1,19
	sw	a1,572(a4)
	li	a1,8
	sw	a1,608(a4)
	li	a1,255
	sw	a1,624(a4)
	addiw	a2,a2,546
	sw	a2,628(a4)
	sw	a3,632(a4)
	li	a3,16384
	addiw	a3,a3,-384
	sw	a3,680(a4)
	li	a3,401408
	addiw	a3,a3,-1408
	sw	a3,684(a4)
	addiw	a5,a5,1911
	sw	a5,816(a4)
	li	a5,5
	sw	a5,824(a4)
	li	a5,1
	sw	a5,516(a4)
	li	a4,268537856
.L3:
	lw	a5,516(a4)
	andi	a5,a5,256
	beqz	a5,.L3

	/* Move _copy_data() to ITCM region */
	li t0, COPY_START_BASE
	lla t1, _copy_data
	lla t2, _copy_data + 4 * 8
	call _copy_data

	/* Flush cache */
	fence
	fence.i
	sfence.vma

	/* Enable I/D cache */
	li t0, CSR_CACHE_ENABLE
	csrs CSR_MCACHE_CTL, t0

	/* UART0: program 8 data bits */
	li t0, 0x10013020
	li t1, 0x30
	sw t1, 0(t0)

	/* Copy OpenSBI from NOR to DDR */
	li t0, OPENSBI_START_BASE
	lla t1, sbi
	lla t2, _end_sbi
	li t3, COPY_START_BASE
	jalr ra, t3, 0

	/* Copy U-Boot from NOR to DDR */
	li t0, UBOOT_START_BASE
	lla t1, uboot
	lla t2, _end_uboot
	li t3, COPY_START_BASE
	jalr ra, t3, 0

	/* Copy FDT from NOR to DDR */
	li t0, FDT_START_BASE
	lla t1, fdt
	lla t2, _end_fdt
	li t3, COPY_START_BASE
	jalr ra, t3, 0

	/* Flush cache */
	fence
	fence.i
	sfence.vma

	/* fw_dynamic entry: a0=hartid, a1=fdt, a2=&fw_dynamic_info */
	li a0, 0
	li a1, FDT_START_BASE
	lla a2, fw_dynamic_info

	/* Jump to OpenSBI */
	li t0, OPENSBI_START_BASE
	jr t0

_deadloop:
	j .

.align 6
exc_entry:
	lla a0, _start
	csrw mepc, a0
	mret

/* copy_data(void *dst, void *src, void *end) */
.align 4
_copy_data:
_loop:
	ld t3, 0(t1)
	sd t3, 0(t0)
	addi t0, t0, 8
	addi t1, t1, 8
	blt t1, t2, _loop
	ret

/* struct fw_dynamic_info — read by OpenSBI at entry before relocation. */
.align 3
.global fw_dynamic_info
.type fw_dynamic_info, @object
fw_dynamic_info:
	.dword FW_DYNAMIC_INFO_MAGIC_VALUE	/* magic */
	.dword FW_DYNAMIC_INFO_VERSION_2	/* version */
	.dword UBOOT_START_BASE			/* next_addr */
	.dword FW_DYNAMIC_INFO_NEXT_MODE_S	/* next_mode = S */
	.dword 0				/* options */
	.dword -1				/* boot_hart = any */

.section .sbipayload
.global sbi
.type sbi, @object
sbi:
.incbin "sbi.bin"


.section .ubootpayload
.global uboot
.type uboot, @object
uboot:
.incbin "u-boot.bin"


.section .fdtpayload
.incbin "fdt.dtb"
