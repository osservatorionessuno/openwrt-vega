# Updating Milk-V Vega without JTAG (in-system sysupgrade)

The board can update itself from the running OpenWrt system over the network —
no JTAG required. This works because the `fsl91030m` driver brings up a working
management path from the CPU to a host cabled to a front-panel copper port
(reachable at `br-lan` = `192.168.1.1`).

> This is the *update* path for a board already running OpenWrt. To install
> OpenWrt on a board still running the vendor firmware, see `INSTALL.md`.

## Method 1 — `sysupgrade` (recommended)

The target builds a combined `…-ubifs-sysupgrade.bin` (a tar of kernel + rootfs
+ metadata). `sysupgrade` pivots to a ramdisk, unmounts the NAND rootfs, and
writes both the kernel (`kernel_nand`, via `mtd`) and the rootfs (`rootfs` UBI
volume, via `ubiupdatevol`) — so the mounted-rootfs problem is handled for you.

1. On a host cabled to **G5**, serve the image (host on the board's subnet, e.g.
   `192.168.1.2`):
   ```sh
   cd <dir-with-images> && python3 -m http.server 8080
   ```
2. On the board, fetch and verify:
   ```sh
   cd /tmp
   wget http://192.168.1.2:8080/openwrt-milkv_vega-milkv_vega-milkv_vega-ubifs-sysupgrade.bin -O s.bin
   md5sum s.bin      # compare with the host
   ```
3. Upgrade:
   ```sh
   sysupgrade -n s.bin      # -n = fresh config; drop -n to keep settings
   ```
4. **Power-cycle** the board after it reboots (see caveat below).

Validated on hardware: `sysupgrade` reflashed kernel + rootfs from the running
system, rebooted into the new image (taint 0), and forwarding returned 10/10
after a power-cycle.

### Bootstrap note
`sysupgrade` runs from the **running** system's `/lib/upgrade/platform.sh`, so a
build that predates sysupgrade support must be updated once by JTAG (or Method 2)
to a sysupgrade-capable image before `sysupgrade` is available.

## Method 2 — manual `mtd` (kernel / NOR only)

For piecewise updates or recovery without a full sysupgrade image:
```sh
mtd write kernel.bin kernel_nand          # kernel — safe, validated
```
- **Root filesystem:** do **not** `mtd write` `ubifs_nand` while it is mounted;
  use Method 1 instead.
- **Bootloader (`vega-spl.bin` → `freeloader`):** `mtd write` works but a bad
  write bricks boot — keep JTAG as the recovery path.

## Caveat: power-cycle, not soft reboot

A soft `reboot` (including the one `sysupgrade` issues) does **not** restore
switch forwarding on this SoC — the fabric keeps sticky state that only a true
power-cycle clears. After any in-system update, **power-cycle the board** for
forwarding to come back.

## What was validated on hardware

- Network pull over the switch to the board (`wget`, md5-exact).
- `mtd write` to NOR (`nor-spare`) and to NAND (`kernel_nand`), md5-exact / boots.
- Full `sysupgrade` (kernel + rootfs) from the running system, then boot +
  forwarding restored after power-cycle.
