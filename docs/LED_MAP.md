# Front-panel LED map

The Milk-V Vega RJ45 G5..G12 front-panel LEDs are CPU-driven via a 74HC164
shift-register chain. **4 cascaded chips -> 32 outputs total.** The chain is
bit-banged through `spi-gpio` (sck = GPIO 12, mosi = GPIO 14). The upstream
`drivers/gpio/gpio-74x164.c` driver handles it cleanly with
`compatible = "fairchild,74hc595"` and `registers-number = <4>`.

**The exposed RJ45 LEDs are active-low** (DT cell flag = 1 =
`GPIO_ACTIVE_LOW`). Setting a bit to 0 lights the LED; bit = 1 turns it off.

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

bits 16..31 are not exposed as production LED class devices.
```

Pattern: ACT at even bit, LINK at odd bit within each RJ45 port pair.
Bindings for G5..G12 are declared in DTS.

## SFP/XGS LEDs

The SFP/SFP+ visible indicators are not exposed as Linux `gpio-leds`.
Runtime observation showed those LEDs activating automatically when the SFP
ports are used, so they are treated as hardware-owned until software control is
proven. Earlier shift-register guesses for XGS1/XGS2 and G1..G4 are not part
of the production LED surface.

## How LEDs are driven from software

The OpenWrt board defaults map the RJ45 switch ports to netdev LED triggers:

```
vega:g5:link   -> g5  link
vega:g5:act    -> g5  tx rx
vega:g6:link   -> g6  link
vega:g6:act    -> g6  tx rx
vega:g7:link   -> g7  link
vega:g7:act    -> g7  tx rx
vega:g8:link   -> g8  link
vega:g8:act    -> g8  tx rx
vega:g9:link   -> g9  link
vega:g9:act    -> g9  tx rx
vega:g10:link  -> g10 link
vega:g10:act   -> g10 tx rx
vega:g11:link  -> g11 link
vega:g11:act   -> g11 tx rx
vega:g12:link  -> g12 link
vega:g12:act   -> g12 tx rx
```

G5 and G12 have been runtime-tested on the current controller cabling. G6..G11
follow the same sequential RJ45 LED bit map and netdev naming convention and
should be validated when those front-panel ports are cabled.

| Static control via sysfs:                                     |
|--------------------------------------------------------------|
| `echo 1 > /sys/class/leds/vega:g5:link/brightness`          (on)   |
| `echo 0 > /sys/class/leds/vega:g5:link/brightness`          (off)  |

| Kernel triggers (autonomous):                                 |
|--------------------------------------------------------------|
| `echo heartbeat > /sys/class/leds/vega:g5:link/trigger`           |
| `echo netdev    > /sys/class/leds/vega:g5:act/trigger`            |
| `echo g5        > /sys/class/leds/vega:g5:act/device_name`        |
| `echo 1         > /sys/class/leds/vega:g5:act/{tx,rx,link}`       |

The `netdev` trigger gives per-netdev blinking driven by the kernel
(rate-limited). With the current switchdev driver, use the Linux-visible
per-port netdevs. G5 and G12 are runtime-tested today; G6..G11 use the same
sequential naming convention for when those ports are exposed and cabled.

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

The RJ45 bit-to-LED mapping was derived empirically by toggling each
shift-register output and observing the panel.
