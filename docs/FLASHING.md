# Flashing OpenWrt to Milk-V Vega

Three files need to be written to non-volatile storage after `./build.sh`:

| File | Target | Notes |
|---|---|---|
| `vega-spl.bin` | NOR @ 0x00000000 | First-stage bootloader. Contains OpenSBI + U-Boot + DTB stitched together. |
| `openwrt-milkv_vega-milkv_vega-milkv_vega-ubifs-kernel.bin` | NAND `kernel_nand` (offset 0x000000, size 8 MiB) | Linux kernel as a uImage |
| `openwrt-milkv_vega-milkv_vega-milkv_vega-ubifs-rootfs.ubi` | NAND `ubifs_nand` (offset 0x800000, size 120 MiB) | UBI volume containing a UBIFS root filesystem |

## Build the JTAG flasher

The repo carries a self-contained flashing kit under `scripts/flashing/vega/`. It
clones the Nuclei `riscv-openocd` fork, checks out the pinned commit, applies
the Vega patches in `scripts/flashing/vega/patches/`, installs `openocd-slow.cfg`,
builds OpenOCD, and generates the NAND helper stubs used by the NAND flash
commands.

```sh
./scripts/flashing/vega/bootstrap.sh
```

Set `SKIP_DEPS=1` if the dependencies are already installed and you do not
want the script to call Homebrew or `apt-get`.

## Flash the images

From the repository root:

```sh
OUT=openwrt/bin/targets/milkv_vega/milkv_vega

./scripts/flashing/vega/flash.sh nor "$OUT/vega-spl.bin"
./scripts/flashing/vega/flash.sh nand kernel "$OUT/openwrt-milkv_vega-milkv_vega-milkv_vega-ubifs-kernel.bin"
./scripts/flashing/vega/flash.sh nand rootfs "$OUT/openwrt-milkv_vega-milkv_vega-milkv_vega-ubifs-rootfs.ubi"
```

The equivalent explicit form is:

```sh
OPENOCD=$PWD/scripts/flashing/vega/riscv-openocd/src/openocd \
OPENOCD_CFG=$PWD/scripts/flashing/vega/riscv-openocd/openocd-slow.cfg \
./scripts/flashing/vega/flash.sh nor "$OUT/vega-spl.bin"
```

## Backup and recovery

To back up the current 4 MiB NOR before flashing:

```sh
./scripts/flashing/vega/dump-flash.sh /tmp/vega-nor.bin 0x400000
```

For a bricked or blank board:

1. Connect the FTDI JTAG adapter and serial console, then power-cycle the
   board.
2. Run `./scripts/flashing/vega/bootstrap.sh` if the patched OpenOCD is not
   already built.
3. Flash `vega-spl.bin` to NOR with `./scripts/flashing/vega/flash.sh nor ...`.
4. Power-cycle again and watch the UART at 115200-8N1.
5. Flash the NAND kernel and rootfs if NAND is empty or from an old build.

Notes:

- The W25Q32 status-register unlock is volatile; repeat flashing commands
  after each power cycle.
- Keep JTAG at or below 8 MHz. Higher clocks have shown DMI corruption.
- Avoid probing `0x40000000+` before DDR is initialized. If the AXI bus wedges,
  power-cycle the board.
