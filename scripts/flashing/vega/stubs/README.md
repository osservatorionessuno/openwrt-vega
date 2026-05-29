# SPI-NAND CPU stubs

Two tiny RISC-V programs that run from ILM (`0x80000000`) under JTAG control
to read or write the SPI-NAND attached to the second NUSPI controller at
`0x10016000`.

Both stubs use only S-mode-safe instructions (no privileged CSRs), no stack,
and ebreak on completion. JTAG sets argument registers and resumes the CPU;
the stub takes over the SPI controller and runs at native CPU speed,
bypassing the byte-at-a-time JTAG bottleneck.

## Build

Normally, run the Vega flashing bootstrap script from the repository root:

```sh
./scripts/flashing/vega/bootstrap.sh
```

It uses the checked-in Python assemblers to generate:

- `nand_read_stub.bin`
- `nand_write_stub.bin`
- `nand_erase_stub.bin`

The binaries are build outputs and are intentionally ignored by git.

You can also build the `.S` versions manually with a RISC-V toolchain:

```sh
CROSS=riscv-nuclei-elf
$CROSS-gcc -nostdlib -nostartfiles -march=rv64imafdc -mabi=lp64 \
    -Wl,-Ttext=0x80000000 -o nand_read_stub.elf  nand_read_stub.S
$CROSS-gcc -nostdlib -nostartfiles -march=rv64imafdc -mabi=lp64 \
    -Wl,-Ttext=0x80000000 -o nand_write_stub.elf nand_write_stub.S
$CROSS-objcopy -O binary nand_read_stub.elf  nand_read_stub.bin
$CROSS-objcopy -O binary nand_write_stub.elf nand_write_stub.bin
```

(With a Linux GCC the `-march`/`-mabi` line is the same; `riscv64-unknown-elf`
or `riscv64-linux-gnu` toolchains work equally well.)

## Argument convention

### `nand_read_stub`
| Reg | Meaning |
|---|---|
| `a0` | SPI base — `0x10016000` |
| `a1` | DRAM destination (must hold `count * 2048` bytes) |
| `a2` | first page index (`0..65535` for 1 Gbit chip) |
| `a3` | page count |

### `nand_write_stub`
| Reg | Meaning |
|---|---|
| `a0` | SPI base — `0x10016000` |
| `a1` | DRAM source (`count * 2048` bytes) |
| `a2` | first page index — block-aligned (`page % 64 == 0`) if erase requested |
| `a3` | page count |
| `a4` | flags — bit 0 = erase blocks first |

## SPI-NAND command sequence (for reference)

| Op | Opcode | Args | Notes |
|---|---|---|---|
| READ_ID | `0x9F` | 1 dummy | returns mfg + dev |
| GET_FEATURES | `0x0F` | reg | reg `0xC0` = STATUS (bit 0 = OIP) |
| PAGE_READ | `0x13` | 3-byte row | loads page to cache |
| READ_FROM_CACHE | `0x0B` | 2-byte col + dummy | streams 2 KiB |
| WRITE_ENABLE | `0x06` | — | required before erase/program |
| PROGRAM_LOAD | `0x02` | 2-byte col + data | fills cache |
| PROGRAM_EXECUTE | `0x10` | 3-byte row | commits cache to NAND |
| BLOCK_ERASE | `0xD8` | 3-byte row (of block start) | erases 128 KiB |
