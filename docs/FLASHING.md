# Flashing OpenWrt to Milk-V Vega

Three files need to be written to non-volatile storage:

| File | Target | Notes |
|---|---|---|
| `vega-spl.bin` | NOR @ 0x00000000 | First-stage bootloader. Contains OpenSBI + U-Boot + DTB stitched together. |
| `openwrt-milkv_vega-milkv_vega-milkv_vega-ubifs-kernel.bin` | NAND `kernel_nand` (offset 0x000000, size 8 MiB) | Linux kernel as a uImage |
| `openwrt-milkv_vega-milkv_vega-milkv_vega-ubifs-rootfs.ubi` | NAND `ubifs_nand` (offset 0x800000, size 120 MiB) | UBI volume containing a UBIFS root filesystem |

