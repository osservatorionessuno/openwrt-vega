#!/usr/bin/env python3
"""Hand-assemble nand_read_stub.S into a raw binary.

Output is byte-identical (modulo trivial code-gen choices) to what a GCC
build of nand_read_stub.S would produce, so we can validate the stub
without a cross-toolchain installed.

Usage: python3 asm_read_stub.py [output.bin]
"""
import struct, sys

# Register numbers
REGS = {
    "zero":0, "ra":1, "sp":2, "gp":3, "tp":4,
    "t0":5, "t1":6, "t2":7,
    "s0":8, "fp":8, "s1":9,
    "a0":10, "a1":11, "a2":12, "a3":13, "a4":14, "a5":15, "a6":16, "a7":17,
    "s2":18, "s3":19, "s4":20, "s5":21, "s6":22, "s7":23,
    "s8":24, "s9":25, "s10":26, "s11":27,
    "t3":28, "t4":29, "t5":30, "t6":31,
}

# --- A miniature RV64I assembler ----------------------------------------

class Asm:
    def __init__(self):
        self.code = []        # list of (kind, ...) records
        self.labels = {}      # label -> instr index

    def _emit(self, rec):     self.code.append(rec); return self
    def label(self, name):    self.labels[name] = len(self.code); return self

    # R-type ops
    def add(self, rd, rs1, rs2):  return self._emit(("R", 0x33, 0, 0, rd, rs1, rs2))
    def sub(self, rd, rs1, rs2):  return self._emit(("R", 0x33, 0, 0x20, rd, rs1, rs2))
    def slli(self, rd, rs1, sh):  return self._emit(("SH", 0x13, 1, sh, rd, rs1))
    def srli(self, rd, rs1, sh):  return self._emit(("SH", 0x13, 5, sh, rd, rs1))

    # I-type ops
    def addi(self, rd, rs1, imm):  return self._emit(("I", 0x13, 0, rd, rs1, imm))
    def andi(self, rd, rs1, imm):  return self._emit(("I", 0x13, 7, rd, rs1, imm))
    def ori (self, rd, rs1, imm):  return self._emit(("I", 0x13, 6, rd, rs1, imm))
    def lw  (self, rd, off, rs1):  return self._emit(("I", 0x03, 2, rd, rs1, off))
    def lbu (self, rd, off, rs1):  return self._emit(("I", 0x03, 4, rd, rs1, off))

    def jalr(self, rd, rs1, off):  return self._emit(("I", 0x67, 0, rd, rs1, off))

    # S-type
    def sw (self, rs2, off, rs1):  return self._emit(("S", 0x23, 2, rs2, rs1, off))
    def sb (self, rs2, off, rs1):  return self._emit(("S", 0x23, 0, rs2, rs1, off))

    # B-type (target is a label)
    def beq (self, rs1, rs2, lbl): return self._emit(("B", 0x63, 0, rs1, rs2, lbl))
    def bne (self, rs1, rs2, lbl): return self._emit(("B", 0x63, 1, rs1, rs2, lbl))
    def blt (self, rs1, rs2, lbl): return self._emit(("B", 0x63, 4, rs1, rs2, lbl))
    def bge (self, rs1, rs2, lbl): return self._emit(("B", 0x63, 5, rs1, rs2, lbl))
    # Pseudo:
    def beqz(self, rs1, lbl):      return self.beq(rs1, 0, lbl)
    def bnez(self, rs1, lbl):      return self.bne(rs1, 0, lbl)
    def bltz(self, rs1, lbl):      return self.blt(rs1, 0, lbl)
    def bgez(self, rs1, lbl):      return self.bge(rs1, 0, lbl)

    # U-type
    def lui (self, rd, imm):       return self._emit(("U", 0x37, rd, imm))

    # J-type (target is a label)
    def jal (self, rd, lbl):       return self._emit(("J", 0x6f, rd, lbl))
    def j   (self, lbl):           return self.jal(0, lbl)
    def ret (self):                return self.jalr(0, 1, 0)

    # Pseudo: load 12-bit immediate
    def li12(self, rd, imm):
        assert -2048 <= imm < 2048, f"{imm} out of range for li12"
        return self.addi(rd, 0, imm)

    # Pseudo: load 32-bit immediate via lui+addi (with sign-extension fixup).
    def li(self, rd, imm):
        imm32 = imm & 0xffffffff
        # if it fits in a 12-bit signed, use a single addi
        if -2048 <= imm < 2048:
            return self.addi(rd, 0, imm)
        lo = imm32 & 0xfff
        # addi sign-extends lo; if lo's bit 11 is set, lo is negative as signed
        if lo & 0x800:
            hi = ((imm32 >> 12) + 1) & 0xfffff
            lo_signed = lo - 0x1000
        else:
            hi = (imm32 >> 12) & 0xfffff
            lo_signed = lo
        self.lui(rd, hi)
        if lo_signed != 0:
            self.addi(rd, rd, lo_signed)
        return self

    # System
    def ebreak(self):              return self._emit(("EBREAK",))

    # ----- assemble ---------------------------------------------------
    def assemble(self):
        out = bytearray()
        for i, rec in enumerate(self.code):
            pc = i * 4
            w = self._encode(rec, pc)
            out += struct.pack("<I", w)
        return bytes(out)

    def _resolve(self, lbl, pc):
        if isinstance(lbl, int): return lbl
        if lbl not in self.labels:
            raise KeyError(f"label {lbl!r} not defined")
        return self.labels[lbl] * 4 - pc

    def _encode(self, rec, pc):
        k = rec[0]
        if k == "I":
            _, op, f3, rd, rs1, imm = rec
            assert -2048 <= imm < 2048, f"I-imm {imm}"
            imm &= 0xfff
            return (imm << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op
        if k == "S":
            _, op, f3, rs2, rs1, imm = rec
            assert -2048 <= imm < 2048
            imm &= 0xfff
            return ((imm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((imm & 0x1f) << 7) | op
        if k == "R":
            _, op, f3, f7, rd, rs1, rs2 = rec
            return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op
        if k == "SH":
            _, op, f3, sh, rd, rs1 = rec
            return (sh << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op
        if k == "B":
            _, op, f3, rs1, rs2, lbl = rec
            off = self._resolve(lbl, pc)
            assert -4096 <= off < 4096 and (off & 1) == 0, f"B-off {off}"
            off &= 0x1fff
            return (((off>>12)&1)<<31) | (((off>>5)&0x3f)<<25) | (rs2<<20) | (rs1<<15) \
                 | (f3<<12) | (((off>>1)&0xf)<<8) | (((off>>11)&1)<<7) | op
        if k == "U":
            _, op, rd, imm = rec
            return ((imm & 0xfffff) << 12) | (rd << 7) | op
        if k == "J":
            _, op, rd, lbl = rec
            off = self._resolve(lbl, pc)
            assert -(1<<20) <= off < (1<<20) and (off & 1) == 0, f"J-off {off}"
            off &= 0x1fffff
            return (((off>>20)&1)<<31) | (((off>>1)&0x3ff)<<21) | (((off>>11)&1)<<20) \
                 | (((off>>12)&0xff)<<12) | (rd<<7) | op
        if k == "EBREAK":
            return 0x00100073
        raise ValueError(k)

# --- The actual program -------------------------------------------------

R = lambda n: REGS[n]

a = Asm()

# page_loop:
a.label("page_loop")
a.beqz(R("a3"), "done")

# ---- PAGE_READ + 3-byte page addr ----
a.li12(R("t0"), 2);    a.sw(R("t0"), 0x18, R("a0"))
a.li12(R("t0"), 1);    a.sw(R("t0"), 0x50, R("a0"))
a.li(R("t0"), 0x80008); a.sw(R("t0"), 0x40, R("a0"))   # FMT len=8 dir=TX

a.li12(R("t0"), 0x13); a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.srli(R("t0"), R("a2"), 16); a.andi(R("t0"), R("t0"), 0xff)
a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.srli(R("t0"), R("a2"), 8);  a.andi(R("t0"), R("t0"), 0xff)
a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.andi(R("t0"), R("a2"), 0xff)
a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")

a.sw(R("zero"), 0x18, R("a0"))   # CS off

# ---- poll OIP via GET_FEATURES 0xC0 ----
a.label("poll_oip")
a.li12(R("t0"), 2);    a.sw(R("t0"), 0x18, R("a0"))
a.li(R("t0"), 0x80008); a.sw(R("t0"), 0x40, R("a0"))

a.li12(R("t0"), 0x0F); a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.li12(R("t0"), -64);  # 0xC0 = 192 doesn't fit in signed 12-bit (range -2048..2047 OK, 192 OK!). Let me use li12(t0, 0xC0) directly.
# correction: 0xC0 = 192 which IS in range. Drop the trick.
# We already pushed the instruction above with t0=-64. Let me overwrite.
# Actually 192 is in range [0..2047], so andi/addi imm signed range allows it.
# Re-emit correctly:
a.code.pop()
a.li12(R("t0"), 0xC0)
a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")

a.label("drain_oip")
a.lw(R("t0"), 0x4c, R("a0"))
a.bgez(R("t0"), "drain_oip")

a.li(R("t0"), 0x80000); a.sw(R("t0"), 0x40, R("a0"))   # FMT len=8 dir=RX
a.sw(R("zero"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.label("wait_oip_rx")
a.lw(R("t0"), 0x4c, R("a0"))
a.bltz(R("t0"), "wait_oip_rx")
a.sw(R("zero"), 0x18, R("a0"))
a.andi(R("t0"), R("t0"), 1)
a.bnez(R("t0"), "poll_oip")

# ---- READ_FROM_CACHE 0x0B + 2-byte col + dummy ----
a.li12(R("t0"), 2);    a.sw(R("t0"), 0x18, R("a0"))
a.li(R("t0"), 0x80008); a.sw(R("t0"), 0x40, R("a0"))

a.li12(R("t0"), 0x0B); a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.sw(R("zero"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.sw(R("zero"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.sw(R("zero"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")

a.label("drain_pre_data")
a.lw(R("t0"), 0x4c, R("a0"))
a.bgez(R("t0"), "drain_pre_data")

a.li(R("t0"), 0x80000); a.sw(R("t0"), 0x40, R("a0"))

# loop reading 2048 bytes
a.li12(R("t1"), 2047)            # use 2047 + addi adjustment? simpler:
a.code.pop()
# 2048 doesn't fit in 12-bit signed (max 2047). Use addi t1, zero, 1; slli t1, t1, 11.
a.addi(R("t1"), R("zero"), 1)
a.slli(R("t1"), R("t1"), 11)
a.addi(R("t2"), R("a1"), 0)     # mv t2, a1

a.label("read_byte_loop")
a.sw(R("zero"), 0x48, R("a0"))   # dummy TX
a.label("rb_wait_tx")
a.lw(R("t0"), 0x74, R("a0"))
a.andi(R("t0"), R("t0"), 1)
a.beqz(R("t0"), "rb_wait_tx")
a.label("rb_wait_rx")
a.lw(R("t0"), 0x4c, R("a0"))
a.bltz(R("t0"), "rb_wait_rx")
a.sb(R("t0"), 0, R("t2"))
a.addi(R("t2"), R("t2"), 1)
a.addi(R("t1"), R("t1"), -1)
a.bnez(R("t1"), "read_byte_loop")

a.sw(R("zero"), 0x18, R("a0"))  # CS off

# advance
# a1 += 2048 -- doesn't fit in 12-bit signed, do it as two adds
a.addi(R("a1"), R("a1"), 1024)
a.addi(R("a1"), R("a1"), 1024)
a.addi(R("a2"), R("a2"), 1)
a.addi(R("a3"), R("a3"), -1)
a.j("page_loop")

# done:
a.label("done")
a.ebreak()
a.j("done")

# wait_tx:
a.label("wait_tx")
a.lw(R("t0"), 0x74, R("a0"))
a.andi(R("t0"), R("t0"), 1)
a.beqz(R("t0"), "wait_tx")
a.ret()

bin_ = a.assemble()
out_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/nand_read_stub.bin"
with open(out_path, "wb") as f:
    f.write(bin_)
print(f"wrote {len(bin_)} bytes to {out_path}")
print(f"labels:")
for name, idx in sorted(a.labels.items(), key=lambda kv: kv[1]):
    print(f"  +0x{idx*4:04x}  {name}")
