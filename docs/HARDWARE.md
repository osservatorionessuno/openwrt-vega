# Hardware notes — Milk-V Vega
## SoC: Fisilink FSL91030M

Contains:
- 1× Nuclei UX608 application core (rv64imafdc + zicsr + zifencei)
- A 14-port Ethernet switch fabric ("XY1000"-branded internally)
- Standard SiFive-IP peripherals: GPIO, UART, SPI, I2C (opencores)
- A PLIC with 53 interrupt lines
- An ACLINT (MSWI + MTimer)

| ID register | Value     |
|---|---|
| `mvendorid` | `0x2d33`   |
| `marchid`   | `0x101c607` |
| `mimpid`    | `0x1020201` |

### Single-hart, Sv39

Linux must be forced into Sv39 (`no4lvl no5lvl` in cmdline). The CPU
does support `riscv,satp-mode = "sv39"`. Other paging modes haven't
been tested but are unlikely to be implemented.

### ASID quirk

UX608's SATP CSR retains writes to the ASID field but the TLB doesn't
honour ASIDs in matching. See `ASID_BUG.md`. The
`200-riscv-disable-asid-allocator.patch` works around this.

## Memory map

```
0x00000000–0x0FFFFFFF  unmapped
0x02000000–0x02000FFF  Nuclei ACLINT MSWI/MTIMER region (timer + IPI)
0x02001000–0x02001FFF  reserved
0x02005000–0x02005007  mtimecmp
0x0200CFF8–0x0200CFFF  mtime
0x08000000–0x0BFFFFFF  PLIC (53 interrupts)
0x10011000–0x10011FFF  SiFive GPIO (32 lines, but only IRQ #1 wired in DT)
0x10012000–0x10012FFF  UART1 (currently disabled)
0x10013000–0x10013FFF  UART0 (console)
0x10014000–0x10014FFF  QSPI0 (SPI NOR boot flash)
0x10016000–0x10016FFF  QSPI1 (SPI NAND)
0x10018000–0x10018FFF  ocores I2C
0x40000000–0x4FFFFFFF  DDR3 (256 MiB)
0x41000000–0x4105FFFF  reserved for OpenSBI
0x67800000–0x67800FFF  Fisilink MAC (xy1000 driver target)
0x68000000–0x6800001F  FSL watchdog
```
