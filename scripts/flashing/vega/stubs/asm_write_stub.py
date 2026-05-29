#!/usr/bin/env python3
"""Hand-assemble nand_write_stub.S into a raw binary.
   Same Assembler class as asm_read_stub.py."""
import struct, sys

REGS = {
    "zero":0, "ra":1, "sp":2, "gp":3, "tp":4,
    "t0":5, "t1":6, "t2":7,
    "s0":8, "fp":8, "s1":9,
    "a0":10, "a1":11, "a2":12, "a3":13, "a4":14, "a5":15, "a6":16, "a7":17,
    "s2":18, "s3":19, "s4":20, "s5":21, "s6":22, "s7":23,
    "s8":24, "s9":25, "s10":26, "s11":27,
    "t3":28, "t4":29, "t5":30, "t6":31,
}

class Asm:
    def __init__(self):
        self.code = []
        self.labels = {}
    def _emit(self, rec):     self.code.append(rec); return self
    def label(self, name):    self.labels[name] = len(self.code); return self
    def add(self, rd, rs1, rs2):  return self._emit(("R", 0x33, 0, 0,   rd, rs1, rs2))
    def sub(self, rd, rs1, rs2):  return self._emit(("R", 0x33, 0, 0x20, rd, rs1, rs2))
    def slli(self, rd, rs1, sh):  return self._emit(("SH", 0x13, 1, sh, rd, rs1))
    def srli(self, rd, rs1, sh):  return self._emit(("SH", 0x13, 5, sh, rd, rs1))
    def addi(self, rd, rs1, imm): return self._emit(("I", 0x13, 0, rd, rs1, imm))
    def andi(self, rd, rs1, imm): return self._emit(("I", 0x13, 7, rd, rs1, imm))
    def ori (self, rd, rs1, imm): return self._emit(("I", 0x13, 6, rd, rs1, imm))
    def lw  (self, rd, off, rs1): return self._emit(("I", 0x03, 2, rd, rs1, off))
    def lbu (self, rd, off, rs1): return self._emit(("I", 0x03, 4, rd, rs1, off))
    def jalr(self, rd, rs1, off): return self._emit(("I", 0x67, 0, rd, rs1, off))
    def sw (self, rs2, off, rs1): return self._emit(("S", 0x23, 2, rs2, rs1, off))
    def sb (self, rs2, off, rs1): return self._emit(("S", 0x23, 0, rs2, rs1, off))
    def beq (self, rs1, rs2, lbl): return self._emit(("B", 0x63, 0, rs1, rs2, lbl))
    def bne (self, rs1, rs2, lbl): return self._emit(("B", 0x63, 1, rs1, rs2, lbl))
    def blt (self, rs1, rs2, lbl): return self._emit(("B", 0x63, 4, rs1, rs2, lbl))
    def bge (self, rs1, rs2, lbl): return self._emit(("B", 0x63, 5, rs1, rs2, lbl))
    def beqz(self, rs1, lbl):  return self.beq(rs1, 0, lbl)
    def bnez(self, rs1, lbl):  return self.bne(rs1, 0, lbl)
    def bltz(self, rs1, lbl):  return self.blt(rs1, 0, lbl)
    def bgez(self, rs1, lbl):  return self.bge(rs1, 0, lbl)
    def lui (self, rd, imm):   return self._emit(("U", 0x37, rd, imm))
    def jal (self, rd, lbl):   return self._emit(("J", 0x6f, rd, lbl))
    def j   (self, lbl):       return self.jal(0, lbl)
    def ret (self):            return self.jalr(0, 1, 0)
    def mv  (self, rd, rs):    return self.addi(rd, rs, 0)
    def li12(self, rd, imm):
        assert -2048 <= imm < 2048
        return self.addi(rd, 0, imm)
    def li(self, rd, imm):
        imm32 = imm & 0xffffffff
        if -2048 <= imm < 2048:
            return self.addi(rd, 0, imm)
        lo = imm32 & 0xfff
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
    def ebreak(self):          return self._emit(("EBREAK",))
    def assemble(self):
        out = bytearray()
        for i, rec in enumerate(self.code):
            w = self._encode(rec, i*4)
            out += struct.pack("<I", w)
        return bytes(out)
    def _resolve(self, lbl, pc):
        if isinstance(lbl, int): return lbl
        if lbl not in self.labels:
            raise KeyError(f"label {lbl!r} not defined")
        return self.labels[lbl]*4 - pc
    def _encode(self, rec, pc):
        k = rec[0]
        if k == "I":
            _, op, f3, rd, rs1, imm = rec
            assert -2048 <= imm < 2048
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
            assert -4096 <= off < 4096 and (off & 1) == 0
            off &= 0x1fff
            return (((off>>12)&1)<<31) | (((off>>5)&0x3f)<<25) | (rs2<<20) | (rs1<<15) \
                 | (f3<<12) | (((off>>1)&0xf)<<8) | (((off>>11)&1)<<7) | op
        if k == "U":
            _, op, rd, imm = rec
            return ((imm & 0xfffff) << 12) | (rd << 7) | op
        if k == "J":
            _, op, rd, lbl = rec
            off = self._resolve(lbl, pc)
            assert -(1<<20) <= off < (1<<20) and (off & 1) == 0
            off &= 0x1fffff
            return (((off>>20)&1)<<31) | (((off>>1)&0x3ff)<<21) | (((off>>11)&1)<<20) \
                 | (((off>>12)&0xff)<<12) | (rd<<7) | op
        if k == "EBREAK":
            return 0x00100073
        raise ValueError(k)

R = lambda n: REGS[n]
a = Asm()

# ---- entry ----
a.andi(R("t3"), R("a4"), 1)
a.beqz(R("t3"), "prog_phase")

# compute first/last block (inclusive/exclusive)
a.srli(R("s0"), R("a2"), 6)
a.add(R("s1"), R("a2"), R("a3"))
a.addi(R("s1"), R("s1"), 63)
a.srli(R("s1"), R("s1"), 6)

# ---- erase loop ----
a.label("erase_loop")
a.beq(R("s0"), R("s1"), "erase_done")

# WRITE_ENABLE
a.li12(R("t0"), 2);    a.sw(R("t0"), 0x18, R("a0"))
a.li12(R("t0"), 1);    a.sw(R("t0"), 0x50, R("a0"))
a.li(R("t0"), 0x80008); a.sw(R("t0"), 0x40, R("a0"))
a.li12(R("t0"), 0x06); a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.sw(R("zero"), 0x18, R("a0"))

# BLOCK_ERASE (0xD8) + 3-byte page-of-block
a.li12(R("t0"), 2);    a.sw(R("t0"), 0x18, R("a0"))
a.li(R("t0"), 0x80008); a.sw(R("t0"), 0x40, R("a0"))
a.li12(R("t0"), 0xD8 - 0x100)   # 0xD8 sign-extended needs care... 0xD8 = 216, fits in 12-bit unsigned but not signed
# Actually 216 > 2047? No, 216 < 2048. It fits in 12-bit signed range [-2048, 2047]. So li12 works.
a.code.pop()
a.li12(R("t0"), 0xD8)
a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.slli(R("t4"), R("s0"), 6)
a.srli(R("t0"), R("t4"), 16); a.andi(R("t0"), R("t0"), 0xff)
a.sw(R("t0"), 0x48, R("a0")); a.jal(R("ra"), "wait_tx")
a.srli(R("t0"), R("t4"), 8);  a.andi(R("t0"), R("t0"), 0xff)
a.sw(R("t0"), 0x48, R("a0")); a.jal(R("ra"), "wait_tx")
a.andi(R("t0"), R("t4"), 0xff)
a.sw(R("t0"), 0x48, R("a0")); a.jal(R("ra"), "wait_tx")
a.sw(R("zero"), 0x18, R("a0"))

a.jal(R("ra"), "wait_oip")
a.addi(R("s0"), R("s0"), 1)
a.j("erase_loop")
a.label("erase_done")

# ---- program phase ----
a.label("prog_phase")
a.mv(R("s0"), R("a1"))   # src
a.mv(R("s1"), R("a2"))   # page
a.mv(R("s2"), R("a3"))   # count

a.label("page_loop")
a.beqz(R("s2"), "all_done")

# WRITE_ENABLE
a.li12(R("t0"), 2);    a.sw(R("t0"), 0x18, R("a0"))
a.li12(R("t0"), 1);    a.sw(R("t0"), 0x50, R("a0"))
a.li(R("t0"), 0x80008); a.sw(R("t0"), 0x40, R("a0"))
a.li12(R("t0"), 0x06); a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.sw(R("zero"), 0x18, R("a0"))

# PROGRAM_LOAD 0x02 + col_hi + col_lo + 2048 data
a.li12(R("t0"), 2);    a.sw(R("t0"), 0x18, R("a0"))
a.li(R("t0"), 0x80008); a.sw(R("t0"), 0x40, R("a0"))
a.li12(R("t0"), 0x02); a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.sw(R("zero"), 0x48, R("a0")); a.jal(R("ra"), "wait_tx")
a.sw(R("zero"), 0x48, R("a0")); a.jal(R("ra"), "wait_tx")

# Inner loop: write 2048 bytes from t2
a.addi(R("t1"), R("zero"), 1)
a.slli(R("t1"), R("t1"), 11)    # t1 = 2048
a.mv(R("t2"), R("s0"))

a.label("prog_byte_loop")
a.lbu(R("t0"), 0, R("t2"))
a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.addi(R("t2"), R("t2"), 1)
a.addi(R("t1"), R("t1"), -1)
a.bnez(R("t1"), "prog_byte_loop")

a.sw(R("zero"), 0x18, R("a0"))

# PROGRAM_EXECUTE 0x10 + 3-byte row
a.li12(R("t0"), 2);    a.sw(R("t0"), 0x18, R("a0"))
a.li(R("t0"), 0x80008); a.sw(R("t0"), 0x40, R("a0"))
a.li12(R("t0"), 0x10); a.sw(R("t0"), 0x48, R("a0"))
a.jal(R("ra"), "wait_tx")
a.srli(R("t0"), R("s1"), 16); a.andi(R("t0"), R("t0"), 0xff)
a.sw(R("t0"), 0x48, R("a0")); a.jal(R("ra"), "wait_tx")
a.srli(R("t0"), R("s1"), 8);  a.andi(R("t0"), R("t0"), 0xff)
a.sw(R("t0"), 0x48, R("a0")); a.jal(R("ra"), "wait_tx")
a.andi(R("t0"), R("s1"), 0xff)
a.sw(R("t0"), 0x48, R("a0")); a.jal(R("ra"), "wait_tx")
a.sw(R("zero"), 0x18, R("a0"))

a.jal(R("ra"), "wait_oip")

# advance: src += 2048, page += 1, count -= 1
a.addi(R("s0"), R("s0"), 1024)
a.addi(R("s0"), R("s0"), 1024)
a.addi(R("s1"), R("s1"), 1)
a.addi(R("s2"), R("s2"), -1)
a.j("page_loop")

a.label("all_done")
a.ebreak()
a.j("all_done")

# ---- wait_oip ----
a.label("wait_oip")
a.mv(R("s11"), R("ra"))          # save ra across internal wait_tx calls

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

a.mv(R("ra"), R("s11"))
a.ret()

# ---- wait_tx ----
a.label("wait_tx")
a.lw(R("t0"), 0x74, R("a0"))
a.andi(R("t0"), R("t0"), 1)
a.beqz(R("t0"), "wait_tx")
a.ret()

bin_ = a.assemble()
out_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/nand_write_stub.bin"
with open(out_path, "wb") as f:
    f.write(bin_)
print(f"wrote {len(bin_)} bytes to {out_path}")
for name, idx in sorted(a.labels.items(), key=lambda kv: kv[1]):
    print(f"  +0x{idx*4:04x}  {name}")
