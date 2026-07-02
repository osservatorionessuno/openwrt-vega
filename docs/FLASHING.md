# Flashing OpenWrt to Milk-V Vega

Three files need to be written to non-volatile storage:

| File | Target | Notes |
|---|---|---|
| `vega-spl.bin` | NOR @ 0x00000000 | First-stage bootloader. Contains OpenSBI + U-Boot + DTB stitched together. |
| `openwrt-milkv_vega-milkv_vega-milkv_vega-ubifs-kernel.bin` | NAND `kernel_nand` (offset 0x000000, size 12 MiB) | Linux kernel as a uImage |
| `openwrt-milkv_vega-milkv_vega-milkv_vega-ubifs-rootfs.ubi` | NAND `ubifs_nand` (offset 0xC00000, size 116 MiB) | UBI volume containing a UBIFS root filesystem |

`kernel_nand` is 12 MiB (not 8) so the recovery **initramfs** image
(`…-initramfs-uImage`, ~11 MiB) also fits in the kernel partition. U-Boot reads
the *actual* uImage size from its header, so the normal kernel still loads only
its own bytes. Any flashing tool that hardcodes offsets must use the new
`ubifs_nand` offset **0xC00000** (was 0x800000).

## NAND Erase Requirement

NAND pages must be erased before programming a replacement kernel or UBI image.
On June 30, 2026, replacing the rootfs by writing only the new image pages and
erasing only the unused tail produced repeatable UBIFS ECC errors in the first
low-numbered UBI eraseblocks and ended in:

```text
VFS: Cannot open root device "ubi0:rootfs" ... error -74
Kernel panic - not syncing: VFS: Unable to mount root fs
```

The corrected procedure is:

- erase NAND blocks `0..95` before writing `kernel_nand` (12 MiB / 128 KiB);
- erase NAND blocks `96..1023` before writing `ubifs_nand`;
- then write the image at the normal partition offset.

(The block ranges reflect the 12 MiB `kernel_nand`; before that change they were
`0..63` / `64..1023` for an 8 MiB kernel partition.)

After erasing the `ubifs_nand` range first, the same rootfs image
`d22e067b394491f760d3bdbed5e5f1459b376c617ed9e2124ea0d755dac40fb8`
mounted cleanly, completed UBIFS free-space fixup, and booted OpenWrt.
