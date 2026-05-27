# Front-panel LED map

All Milk-V Vega front-panel LEDs are CPU-driven via a 74HC164
shift-register chain. **4 cascaded chips → 32 outputs total.** The chain
is bit-banged through `spi-gpio` (sck = GPIO 12, mosi = GPIO 14). The
upstream `drivers/gpio/gpio-74x164.c` driver handles it cleanly with
`compatible = "fairchild,74hc595"` and `registers-number = <4>`.

**All panel LEDs are active-low** (DT cell flag = 1 = `GPIO_ACTIVE_LOW`).
Setting a bit to 0 lights the LED; bit = 1 turns it off.

## Empirically-verified bit map

Established by walking each bit on a real board, 2026-05-26.

```
RJ45 1G ports G5..G12 (front-panel order, leftmost = G12):
  bit  0  G12 act      bit  1  G12 link
  bit  2  G11 act      bit  3  G11 link
  bit  4  G10 act      bit  5  G10 link
  bit  6  G9  act      bit  7  G9  link
  bit  8  G8  act      bit  9  G8  link
  bit 10  G7  act      bit 11  G7  link
  bit 12  G6  act      bit 13  G6  link
  bit 14  G5  act      bit 15  G5  link

SFP+ 10G ports XGS1, XGS2:
  bit 16  XGS1 act     bit 17  XGS1 link  (see caveat)
  bit 18  XGS2 act     bit 19  XGS2 link  (see caveat)

  bit 20  unmapped

SFP 1G ports G1..G4:
  bit 21  G2 act       bit 22  G2 link    (see caveat)
  bit 23  G1 act       bit 24  G1 link    (see caveat)
  bit 25  G4 act       bit 26  G4 link    (see caveat)
  bit 27  G3 act       bit 28  G3 link    (see caveat)

  bits 29..31  unmapped
```

Pattern: ACT at even bit, LINK at odd bit within each port pair, both
for RJ45 and SFP cages. Bindings for all 28 LEDs are declared in DTS.

## Caveat — SFP link LEDs

The 6 SFP link bits (17, 19, 22, 24, 26, 28) toggle their shift-register
outputs cleanly but did not light a visible LED on the dev board we
empirically walked. The mapping above is the natural-pattern guess
(adjacent to each ACT bit, same ordering convention as the RJ45
ports). On a production board with all LEDs populated and intact
traces, these bindings should drive the right physical LEDs.

## How LEDs are driven from software

| Static control via sysfs:                                     |
|--------------------------------------------------------------|
| `echo 1 > /sys/class/leds/vega:g5:link/brightness`          (on)   |
| `echo 0 > /sys/class/leds/vega:g5:link/brightness`          (off)  |

| Kernel triggers (autonomous):                                 |
|--------------------------------------------------------------|
| `echo heartbeat > /sys/class/leds/vega:g5:link/trigger`           |
| `echo netdev    > /sys/class/leds/vega:g5:act/trigger`            |
| `echo eth0      > /sys/class/leds/vega:g5:act/device_name`        |
| `echo 1         > /sys/class/leds/vega:g5:act/{tx,rx,link}`       |

The `netdev` trigger gives per-netdev blinking driven by the kernel
(rate-limited). To get per-port behaviour rather than aggregate-on-eth0,
we'd need a switchdev/DSA driver for XY1000 — see `docs/TODO.md`.

## Extracting the GPIO numbers (provenance)

The four module-parameter GPIO numbers used by the shift-register chain
(`gpio_clk` = 12, `gpio_data` = 14, `gpio_rst` = 18, `gpio_preventloop`
= 20) were extracted from the vendor's binary `led_164.ko`:

```sh
riscv64-linux-gnu-objdump -t /path/to/led_164.ko | grep -E 'gpio_'
# yields the .sdata offset of each module_param variable
riscv64-linux-gnu-objdump -s -j .sdata /path/to/led_164.ko
# .sdata content (little-endian, 4 bytes per var, in symbol declaration order):
#   14 00 00 00  12 00 00 00  0e 00 00 00  0c 00 00 00
#       = 20            18            14            12
```

The bit-to-LED mapping was derived empirically by toggling each of the
32 shift-register outputs and observing the panel.
