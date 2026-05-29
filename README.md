# OpenWrt port for Milk-V Vega

OpenWrt target port for the **Milk-V Vega** a 14-port RISC-V switch SBC.
Currently booting OpenWrt works, and is stable. However there's no switch
driver, though there's one network interface available for management.

## Hardware summary

- **SoC:** Fisilink FSL91030M
- **RAM:** 256 MiB
- **NOR flash:** 4 MiB SPI NOR (Winbond W25Q32)
- **NAND flash:** 128 MiB SPI NAND (GigaDevice)
- **Ports:**
  - 8× 1 GbE copper (RJ45), labelled G5..G12
  - 4× 1 G SFP, labelled G1..G4
  - 2× 10 G SFP+, labelled XGS1, XGS2

## Quick start

```sh
./build.sh
```

Clones OpenWrt at a known-good commit, overlays this repo's files, and runs
`make`. Output goes to `openwrt/bin/targets/milkv_vega/milkv_vega/`.

## Flashing and recovery

This repo carries the JTAG flashing kit under `scripts/flashing/vega/`. It
clones the Nuclei `riscv-openocd` fork, applies the Vega patches, builds
OpenOCD, and provides scripts for NOR SPL recovery and NAND kernel/rootfs
flashing.

```sh
./scripts/flashing/vega/bootstrap.sh

OUT=openwrt/bin/targets/milkv_vega/milkv_vega
./scripts/flashing/vega/flash.sh nor "$OUT/vega-spl.bin"
./scripts/flashing/vega/flash.sh nand kernel "$OUT/openwrt-milkv_vega-milkv_vega-milkv_vega-ubifs-kernel.bin"
./scripts/flashing/vega/flash.sh nand rootfs "$OUT/openwrt-milkv_vega-milkv_vega-milkv_vega-ubifs-rootfs.ubi"
```

Explicit OpenOCD environment form:

```sh
OPENOCD=$PWD/scripts/flashing/vega/riscv-openocd/src/openocd \
OPENOCD_CFG=$PWD/scripts/flashing/vega/riscv-openocd/openocd-slow.cfg \
./scripts/flashing/vega/flash.sh nor /path/to/vega-spl.bin
```

For a blank or bricked board, power-cycle, flash `vega-spl.bin` to NOR first,
power-cycle again, then flash the NAND kernel and rootfs. See
`docs/FLASHING.md` for backup, UART, and recovery notes.

## What this port carries

### Kernel patches (`target/linux/milkv_vega/patches-6.12/`)

| Patch | Purpose |
|---|---|
| `001-riscv-dts-add-fisilink-subdir.patch`  | Adds `arch/riscv/boot/dts/fisilink/` to the DT include path so the kernel DTB build sees our DTS |
| `200-riscv-disable-asid-allocator.patch`    | Disables the ASID allocator on this SoC. The mainline ASID allocator (commit `65d4b9c53017`, Linux v5.12) probes SATP for ASID bits and trusts the result, but UX608 retains writes to the ASID field while its TLB doesn't actually distinguish by ASID. This causes silent memory corruption across context switches — manifests as SIGSEGV after `wait4()` returns in user space. See `docs/ASID_BUG.md`. |
| `210-add-xy1000-net-driver.patch`           | Adds the vendor `xy1000_net.c` (Fisilink MAC driver) to the kernel build. Source file lives under `files-6.12/drivers/net/ethernet/xy1000/`. Minor 6.12 API touch-ups applied (return types, `eth_hw_addr_set`, `static`/`__maybe_unused`). |


### Device tree (`package/boot/vega-spl/src/milkv-vega.dts`)

Lives in the SPL package because the SPL incbins the DTB and hands it to
OpenSBI → U-Boot → Linux. Notable bits:

- `/chosen/rng-seed` — 256-bit bootstrap entropy seed. UX608 has no Zkr
  extension and idle IRQ rate is too low to organically seed the kernel
  RNG before procd's `ubusd` tries `getrandom()`. Linux 6.12 defaults
  `random.trust_bootloader=true`, so the seed marks the pool initialized
  immediately at boot. Replaced by per-device randomness after first boot.
- 32 PLIC IRQs declared on the SiFive GPIO node — the upstream
  `gpio-sifive` driver derives line count from declared parent-IRQ count
  (one IRQ per GPIO line, matching SiFive HiFive Unleashed). Vega's
  vendor DT only declared one. We follow the Canaan K210 pattern and
  declare 32, claiming PLIC lines `1` (real) plus `13..43` (padding).
- LED bindings for the 32-output 74HC164 shift register chain.

### Userspace

- `target/linux/milkv_vega/base-files/` — standard OpenWrt
  per-target overlay

### Flashing kit (`scripts/flashing/vega/`)

- `bootstrap.sh` clones the pinned Nuclei OpenOCD fork, applies the
  `scripts/flashing/vega/patches/` speed/recovery patches, builds OpenOCD, and
  generates the NAND helper stubs.
- `flash.sh` flashes the NOR SPL, NAND kernel, and NAND rootfs using
  the patched OpenOCD.
- `configs/openocd-board.cfg` contains the Vega FTDI/JTAG and slow-path NOR
  flash configuration that `bootstrap.sh` installs as
  `riscv-openocd/openocd-slow.cfg`.

## Documentation

- `docs/FLASHING.md` — how to build the flasher, write the three images, and recover a board
- `docs/ASID_BUG.md` — the silent memory corruption bug we bisected
- `docs/LED_MAP.md` — empirical bit-to-LED map of the 32-output chain
- `docs/HARDWARE.md` — what we know about the SoC and board

## License

This port glue is whatever OpenWrt's terms are (GPL-2.0 for kernel pieces,
package-specific for the rest). The vendor `xy1000_net.c` is `GPL` per
its `MODULE_LICENSE`.
