#!/usr/bin/env python3
"""Hand-assemble a small SPI-NAND erase-only stub.

Inputs (set via JTAG before resume):
  a0 = SPI base (0x10016000)
  a1 = first block
  a2 = last block (exclusive)
"""
import struct, sys, importlib.util
from pathlib import Path

# Reuse the Asm class from asm_read_stub.py to avoid duplication
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "asm_read_stub", HERE / "asm_read_stub.py")
mod = importlib.util.module_from_spec(spec)
# Don't execute its program body — only need Asm + REGS
mod_text = Path(spec.origin).read_text()
# Strip the "actual program" part (everything after "# --- The actual program")
header = mod_text.split("# --- The actual program")[0]
exec(header, mod.__dict__)
Asm, REGS = mod.Asm, mod.REGS

R = lambda n: REGS[n]
a = Asm()

# erase_loop: while a1 != a2: erase block a1; a1++
a.label("erase_loop")
a.beq(R("a1"), R("a2"), "all_done")

# WRITE_ENABLE (0x06)
a.li12(R("t0"), 2);    a.sw(R("t0"), 0x18, R("a0"))
a.li12(R("t0"), 1);    a.sw(R("t0"), 0x50, R("a0"))
a.li(R("t0"), 0x80008); a.sw(R("t0"), 0x40, R("a0"))
a.li12(R("t0"), 0x06); a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.sw(R("zero"), 0x18, R("a0"))

# BLOCK_ERASE (0xD8) + 3-byte page-of-block
a.li12(R("t0"), 2);    a.sw(R("t0"), 0x18, R("a0"))
a.li(R("t0"), 0x80008); a.sw(R("t0"), 0x40, R("a0"))
a.li12(R("t0"), 0xD8); a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.slli(R("t4"), R("a1"), 6)
a.srli(R("t0"), R("t4"), 16); a.andi(R("t0"), R("t0"), 0xff)
a.sw(R("t0"), 0x48, R("a0")); a.jal(R("ra"), "wait_tx")
a.srli(R("t0"), R("t4"), 8);  a.andi(R("t0"), R("t0"), 0xff)
a.sw(R("t0"), 0x48, R("a0")); a.jal(R("ra"), "wait_tx")
a.andi(R("t0"), R("t4"), 0xff)
a.sw(R("t0"), 0x48, R("a0")); a.jal(R("ra"), "wait_tx")
a.sw(R("zero"), 0x18, R("a0"))

a.jal(R("ra"), "wait_oip")
a.addi(R("a1"), R("a1"), 1)
a.j("erase_loop")

a.label("all_done")
a.ebreak()
a.j("all_done")

# wait_oip (preserves ra via s11)
a.label("wait_oip")
a.addi(R("s11"), R("ra"), 0)
a.label("oip_again")
a.li12(R("t0"), 2);    a.sw(R("t0"), 0x18, R("a0"))
a.li(R("t0"), 0x80008); a.sw(R("t0"), 0x40, R("a0"))
a.li12(R("t0"), 0x0F); a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.li12(R("t0"), 0xC0); a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.label("oip_drain")
a.lw(R("t0"), 0x4c, R("a0"))
a.bgez(R("t0"), "oip_drain")
a.li(R("t0"), 0x80000); a.sw(R("t0"), 0x40, R("a0"))
a.sw(R("zero"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.label("oip_wait_rx")
a.lw(R("t0"), 0x4c, R("a0"))
a.bltz(R("t0"), "oip_wait_rx")
a.sw(R("zero"), 0x18, R("a0"))
a.andi(R("t0"), R("t0"), 1)
a.bnez(R("t0"), "oip_again")
a.addi(R("ra"), R("s11"), 0)
a.ret()

# wait_tx
a.label("wait_tx")
a.lw(R("t0"), 0x74, R("a0"))
a.andi(R("t0"), R("t0"), 1)
a.beqz(R("t0"), "wait_tx")
a.ret()

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/nand_erase_stub.bin"
b = a.assemble()
open(out,"wb").write(b)
print(f"wrote {len(b)} bytes to {out}")
