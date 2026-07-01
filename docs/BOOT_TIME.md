# Milk-V Vega Boot-Time Notes

## NAND Kernel Read

Measured on June 29, 2026 with `vega measure-boot` against the production
OpenWrt image:

| boot path | NAND read | userspace |
| --- | ---: | ---: |
| baseline, U-Boot reads `0x800000` | 44.61 s / 44.61 s | 114.76 s / 114.63 s |
| one-shot U-Boot test, reads `0x580000` | 32.09 s to `Starting kernel` | booted normally |
| production fix, U-Boot reads `0x600000` | 33.76 s / 33.76 s | 103.75 s / 103.78 s |
| deferred fast raw reader, reads `0x600000` | 5.31 s U-Boot read, kernel starts at 11.33 s / 11.35 s | 76.53 s / 76.63 s |

The original boot command read the full 8 MiB `kernel_nand` partition:

```text
mtd list && mtd read kernel_nand 0x42000000 0 0x800000 && bootm 0x42000000 - ${fdtcontroladdr}
```

The current gzip uImage is 5,684,147 bytes including the 64-byte legacy uImage
header. Reading the full 8 MiB partition therefore wasted about 2.70 MiB on
every boot through a slow U-Boot SPI-NAND/MTD path.

The first production boot-time fix changed the generic U-Boot MTD read to 6 MiB:

```text
mtd list && mtd read kernel_nand 0x42000000 0 0x600000 && bootm 0x42000000 - ${fdtcontroladdr}
```

`target/linux/milkv_vega/image/Makefile` sets `KERNEL_SIZE := 6144k`, so future
kernel images larger than the U-Boot read window fail the OpenWrt image build
instead of silently producing an unbootable image.

Validated artifact:

```text
runtime-artifacts/boot-read-6m-20260629T153536Z/vega-spl.bin
sha256 09021209ecfbd6740162cc328c48c041e91a98351ec1d18dbd80b445dbee2834
```

After flashing this NOR image, the serial log confirms:

```text
Reading 6291456 byte(s) (3072 page(s)) at offset 0x00000000
```

## Deferred Fast Raw Reader

The controller NAND flasher does not use U-Boot's generic MTD reader. It loads
a small RISC-V stub over JTAG, programs the SPI controller directly, and has the
stub read/write NAND from DDR. The write path is end-to-end limited mostly by
JTAG `load_image`; the target-side NAND stub itself is much faster.

Measured comparison for the same 6 MiB / 3072-page kernel window:

| path | target NAND read | host-visible elapsed |
| --- | ---: | ---: |
| U-Boot generic `mtd read`, 6 MiB | 33.76 s / 33.76 s | same, boot path |
| controller JTAG read stub | 5 s internal stub time | 44.689 s including JTAG dump-back |
| U-Boot `vega_nand_bootread` | 5309 ms / 5310 ms | boot path |

An experimental U-Boot reader was tested, then deliberately left out of the
consolidated patchset because it is board-specific raw SPI-NAND code rather than
a generic U-Boot/SPI-MEM fix. The tested patch was:

```text
package/boot/uboot-milkv_vega/patches/0003-board-fisilink-add-fast-spi-nand-boot-reader.patch
```

It is intentionally narrow: it reads raw 2048-byte pages from the first 8 MiB
kernel partition window, using the same SPI-NAND sequence as the known-good
controller stub:

- `0x13` page read to NAND cache;
- `0x0f 0xc0` status poll until busy clears;
- `0x0b` fast read from cache into DDR.

The command requires the SPI-NAND device to be probed first. A first attempt
without `mtd list` returned to the U-Boot prompt before loading the kernel.
Running `mtd list` first made a one-page read produce a valid uImage header, and
a full 6 MiB read verified cleanly with `iminfo`.

The tested experimental boot command was:

```text
mtd list && vega_nand_bootread 0x42000000 0 0x600000 && bootm 0x42000000 - ${fdtcontroladdr}
```

Validated experimental artifact:

```text
runtime-artifacts/fast-nand-bootread-probe-20260629T1835Z/vega-spl.bin
sha256 ad458435eabefb4f833cd1184328bc73f25e1d4670abe47ab22e7df5596f1d9a
```

Serial validation:

```text
Vega fast NAND: read 6291456 bytes (3072 pages) to 0x42000000 in 5310 ms
Verifying Checksum ... OK
Starting kernel ...
```

Runtime sanity after the experimental NOR update:

- OpenWrt boots Linux 6.12.89.
- `br-lan` contains `g5` and `g12`.
- G5 to G12 and G12 to G5 controller pings both pass with 0% loss.
- No FSL driver fault, panic, oops, call trace, DMA error, or register verify
  failure was found in the checked kernel log.

## Remaining Options

The consolidated source keeps the generic U-Boot SPI-NAND path with the 6 MiB
read-window fix:

```text
mtd list && mtd read kernel_nand 0x42000000 0 0x600000 && bootm 0x42000000 - ${fdtcontroladdr}
```

Possible next investigations:

- move the fast-read win into U-Boot's generic SiFive SPI / SPI-MEM / SPI-NAND
  path instead of carrying board-local raw NAND code;
- test `sckdiv=1` in a rollback-ready experimental fast reader; the tested
  reader deliberately kept the controller-stub-proven
  `sckdiv=3`;
- test a smaller kernel compression format such as LZMA after enabling the
  matching U-Boot decompressor;
- reduce the remaining kernel/rootfs/userspace time, which is now the dominant
  boot cost.
