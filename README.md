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

## Documentation

- `docs/FLASHING.md` — how to write the three images to NOR/NAND
- `docs/ASID_BUG.md` — the silent memory corruption bug we bisected
- `docs/LED_MAP.md` — empirical bit-to-LED map of the 32-output chain
- `docs/HARDWARE.md` — what we know about the SoC and board

## License

This port glue is whatever OpenWrt's terms are (GPL-2.0 for kernel pieces,
package-specific for the rest). The vendor `xy1000_net.c` is `GPL` per
its `MODULE_LICENSE`.
