"""
emu_fixes.py -- the three correctness fixes primeLua needs from primetcc's
ARMv5 emulator, applied by monkey-patching at import time.

WHY A PATCH AND NOT AN EDIT
    tests/harness_lua.py drives lua.elf on primetcc's Python emulator
    (primetcc/tests/emu_arm2.py), which is a *sibling project's* test helper.
    primeLua does not own that file, so the fixes live here instead: importing
    this module wraps the emulator's methods, and primetcc's checkout stays
    byte-for-byte as primetcc left it.

    The three defects are real, and each was found by running the same ARM code
    under qemu-arm (an independent, industrial ARM implementation) and under
    this emulator and diffing the results:

    1. ARMv5TE DSP multiplies (SMULxy / SMLAxy / SMLALxy / SMULWx / SMLAWx)
       are not implemented.  Bits 27-26 of these are 00, so an unimplemented
       one falls through to the data-processing case and is executed as a
       different instruction *without any error*.  GCC emits them for
       -mcpu=arm926ej-s -O2 -- `era * 400` in primeLua's date code becomes one
       `smlabb` -- which made every date come out in the year 370.

    2. LDM with the base register in the load list (`pop {sp, pc}` is the
       everyday form, and libgcc's 64-bit helpers are full of it) applied the
       writeback on top of the value just loaded, clobbering it.  Architecturally
       the old Rn value is unpredictable on ARMv5, but every real core and every
       compiler treat the load as winning (which is what ARMv7 documents) --
       qemu does, and the emulator did not: 64-bit division by a non-constant
       divisor looped forever.

    3. Post-indexed LDRD/STRD (P=0, W=0) did not write back.  In these
       instructions the W bit is not the writeback flag: the post-indexed form
       is encoded W=0.  `*p++` over an array of `long long` compiles to
       `ldrd r0, [r3], #8`, so the pointer never advanced and a loop over such
       an array re-read its first element forever.

    5. The firmware's get_event stub in the slot's "original" 16 bytes was
       modelled wrong -- one word of stub plus padding:

           0xe52d0004  push {r0}
           0xe52de004  push {lr}
           0xef01003f  svc 0x1003F
           0xe49d0004  pop  {r0}
           0xe49df004  pop  {pc}
           0x00000000  (padding)

       The naive model (push, svc, lr-return) hid a real bug for a long time:
       the kernel takes the SVC's return address FROM THE STACK, so a hook that
       CALLS the stub (blx) with its own frame on top returns to garbage.  Only
       the real 6-word shape reproduces the device, where "enter the stub, its
       own pop {pc} comes back to you" is the only thing that works.  Fixing the
       model is what let the device bug be reproduced and fixed at all.

    4. load_elf() reads a segment's file bytes from `p_vaddr` instead of
       `p_offset`.  Those are equal only while the linker keeps the file and
       memory layouts identical; binutils aligns the second PT_LOAD to
       MAXPAGE (64 KB), so lua.elf's data segment has p_offset 0x5bea0 and
       p_vaddr 0x6bea0 -- a 0x10000 gap.  The emulator then loads 64 KB of
       unrelated bytes over every global and applies the dynamic relocations
       64 KB off, so `__hp_input`'s "PRIMEIN" never appears where the hook's
       relocated literal pool points at it.  The device is unaffected (the
       launcher copies `p_offset..p_offset+p_filesz` to p_vaddr, which is what
       the field means); the patch below makes the emulator obey the field too.

    Each method below is primetcc's own implementation with the one-line
    correction; if primetcc's emulator is updated, re-check these copies.

USAGE
    import emu_fixes          # after `import emu_arm2` from primetcc/tests
"""
import emu_arm2

MASK = emu_arm2.MASK

# ---------------------------------------------------------------------------
# 1. ARMv5TE DSP multiplies
# ---------------------------------------------------------------------------
DSP_PATTERNS = (0x01600080,      # SMUL<x><y>   Rd, Rn, Rm
                0x01000080,      # SMLA<x><y>   Rd, Rn, Rm, Ra
                0x01400080,      # SMLAL<x><y>  RdLo, RdHi, Rn, Rm
                0x01200080,      # SMLAW<y>     Rd, Rn, Rm, Ra
                0x012000A0)      # SMULW<y>     Rd, Rn, Rm


def _dsp_mul(self, insn):
    """ARMv5TE signed multiply / multiply-accumulate on halfwords.

        SMUL<x><y>  Rd, Rn, Rm          Rd = Rn.<x> * Rm.<y>
        SMLA<x><y>  Rd, Rn, Rm, Ra      Rd = Rn.<x> * Rm.<y> + Ra
        SMLAL<x><y> RdLo, RdHi, Rn, Rm  RdHi:RdLo += Rn.<x> * Rm.<y>
        SMULW<y>    Rd, Rn, Rm          Rd = (Rn * Rm.<y>) >> 16
        SMLAW<y>    Rd, Rn, Rm, Ra      Rd = ((Rn * Rm.<y>) >> 16) + Ra

    x selects the halfword of the register in bits 3-0 and y the halfword of
    the register in bits 11-8 (checked against arm-none-eabi-as output).  The Q
    saturation flag is not modelled: GCC never tests it, and these instructions
    leave NZCV alone, which is what this does.
    """
    op = (insn >> 20) & 0xFF
    rd = (insn >> 16) & 0xF
    ra = (insn >> 12) & 0xF
    rs = (insn >> 8) & 0xF
    rn = insn & 0xF
    x = (insn >> 5) & 1
    y = (insn >> 6) & 1

    def sx16(v):
        v &= 0xFFFF
        return v - 0x10000 if v & 0x8000 else v

    def sx32(v):
        return v - 0x100000000 if v & 0x80000000 else v

    def half(v, top):
        return sx16(v >> 16) if top else sx16(v)

    if op == 0x12:                       # SMULW<y> / SMLAW<y>
        word = sx32(self.regs[rn])
        h = half(self.regs[rs], y)
        p = (word * h) >> 16
        if ((insn >> 4) & 0xF) == 0xA:   # SMULW<y>
            self.regs[rd] = p & MASK
        else:                            # SMLAW<y>
            self.regs[rd] = (p + self.regs[ra]) & MASK
        return

    p = half(self.regs[rn], x) * half(self.regs[rs], y)
    if op == 0x16:                       # SMUL<x><y>
        self.regs[rd] = p & MASK
    elif op == 0x10:                     # SMLA<x><y>
        self.regs[rd] = (p + self.regs[ra]) & MASK
    else:                                # SMLAL<x><y>: RdHi = bits 19-16
        acc = ((self.regs[rd] << 32) | self.regs[ra]) + p
        self.regs[ra] = acc & MASK
        self.regs[rd] = (acc >> 32) & MASK


_orig_exec = emu_arm2.ARMEmu.exec


def _exec(self, insn, addr):
    if (insn & 0x0FF000F0) in DSP_PATTERNS:
        if self.cond_ok((insn >> 28) & 0xF):
            _dsp_mul(self, insn)
        return
    _orig_exec(self, insn, addr)


# ---------------------------------------------------------------------------
# 2. LDM whose base register is in the load list
# ---------------------------------------------------------------------------
def _ldm_stm(self, insn):
    """primetcc's ldm_stm with the writeback correction (see the module doc)."""
    l = (insn >> 20) & 1
    p = (insn >> 24) & 1
    u = (insn >> 23) & 1
    w = (insn >> 21) & 1
    rn = (insn >> 16) & 0xF
    reglist = insn & 0xFFFF
    addr = self.regs[rn]
    regs = []
    if u:
        for r in range(16):
            if reglist & (1 << r):
                regs.append(r)
    else:
        for r in range(15, -1, -1):
            if reglist & (1 << r):
                regs.append(r)
    for r in regs:
        if p:
            addr = (addr + 4) & MASK if u else (addr - 4) & MASK
        if l:
            self.regs[r] = self.rd32(addr)
        else:
            self.wr32(addr, self.regs[r])
        if not p:
            addr = (addr + 4) & MASK if u else (addr - 4) & MASK
    # FIX: a loaded value for the base register wins over the writeback.
    if w and not (l and (reglist & (1 << rn))):
        self.regs[rn] = addr
    if l and (reglist & 0x8000):
        self.regs[15] = self.regs[15] & ~1


# ---------------------------------------------------------------------------
# 3. post-indexed LDRD / STRD
# ---------------------------------------------------------------------------
def _ldrd_strd(self, insn, addr, is_load):
    """primetcc's ldrd_strd with the writeback correction (see the doc)."""
    p = (insn >> 24) & 1
    u = (insn >> 23) & 1
    w = (insn >> 21) & 1
    rn = (insn >> 16) & 0xF
    rt = (insn >> 12) & 0xF
    imm = (((insn >> 8) & 0xF) << 4) | (insn & 0xF)
    off = imm if u else -imm
    base = (addr + 8) if rn == 15 else self.regs[rn]
    ea = base if not p else (base + off) & MASK
    if ea % 8 != 0:
        raise RuntimeError(
            'unaligned LDRD/STRD: pc=0x%08X insn=%08X ea=0x%08X' %
            (addr, insn, ea))
    if is_load:
        self.regs[rt] = self.rd32(ea)
        self.regs[rt + 1] = self.rd32(ea + 4)
    else:
        self.wr32(ea, self.regs[rt])
        self.wr32(ea + 4, self.regs[rt + 1])
    # FIX: the post-indexed form is P=0 W=0 -- W is not the writeback flag in
    # LDRD/STRD, so keying off W alone loses the pointer advance.
    if w or not p:
        self.regs[rn] = (base + off) & MASK


def _real_slot_stub(self):
    """Install the device's REAL get_event stub bytes in the firmware slot.

    Called once per emulator: the slot's original content is what an input hook
    copies as its trampoline, so this must match the device or every hook test
    tests the model instead of the machine."""
    words = (0xE52D0004,      # push {r0}
             0xE52DE004,      # push {lr}
             0xEF01003F,      # svc 0x1003F   (return address: from the stack)
             0xE49D0004,      # pop  {r0}
             0xE49DF004,      # pop  {pc}
             0x00000000)
    for off, w in enumerate(words):
        self.wr32(SLOT_ADDR + 4 * off, w)


SLOT_ADDR = 0x307FBFA0


_orig_emu_init = emu_arm2.ARMEmu.__init__


def _emu_init(self, *a, **k):
    _orig_emu_init(self, *a, **k)
    _real_slot_stub(self)


def apply():
    emu_arm2.ARMEmu.__init__ = _emu_init
    emu_arm2.ARMEmu.exec = _exec
    emu_arm2.ARMEmu.ldm_stm = _ldm_stm
    emu_arm2.ARMEmu.ldrd_strd = _ldrd_strd
    emu_arm2.ARMEmu.load_elf = _load_elf
    emu_arm2.Firmware.call = _fw_call


# ---------------------------------------------------------------------------
# 5. calloc (svc 0x10038): the firmware's ZEROED allocation
#
# primeLua's copy of this file does not need it; nbCode does.  nbCode asks the
# firmware for its image block with calloc, because the image's .bss then
# arrives zeroed -- 116 KB that would otherwise be written one word at a time
# through the debug channel.  primetcc's emulator models malloc and free and
# stops there, so without this the service would come back as "unknown svc
# 0x10038" and the loader would fall back to the slow path every time: the test
# would pass while the device did something else.
#
# The service is real (primetcc's own service list: "0x10037 malloc  0x10038
# calloc  0x10039 realloc  0x1003a free"), and the model is the obvious one:
# malloc, then zero it.
# ---------------------------------------------------------------------------
_orig_fw_call = emu_arm2.Firmware.call


def _fw_call(self, num, r, emu):
    if num == 0x10038:                          # calloc(n, size)
        total = r[0] * r[1]
        r[0] = total
        _orig_fw_call(self, 0x10037, r, emu)    # malloc(total)
        addr = r[0]
        if addr and total:
            base = emu.base if hasattr(emu, 'base') else emu_arm2.BASE
            addr -= base
            emu.mem[addr:addr + total] = b'\x00' * total
        return
    return _orig_fw_call(self, num, r, emu)


# ---------------------------------------------------------------------------
# 4. load_elf: a PT_LOAD's file bytes live at p_offset, not p_vaddr
# ---------------------------------------------------------------------------
def _load_elf(self, elf, base):
    """primetcc's load_elf with p_offset honoured (see the module doc).

    Only the PT_LOAD copy changes; the dynamic-relocation walk below is
    primetcc's, and it is correct because `r_offset` really is a virtual
    address."""
    import struct
    e_phoff = struct.unpack_from('<I', elf, 28)[0]
    e_phentsize = struct.unpack_from('<H', elf, 42)[0]
    e_phnum = struct.unpack_from('<H', elf, 44)[0]
    e_entry = struct.unpack_from('<I', elf, 24)[0]
    dyn_off = None
    for i in range(e_phnum):
        ph = elf[e_phoff + i * e_phentsize: e_phoff + (i + 1) * e_phentsize]
        p_type, p_off, p_vaddr, _p_paddr, p_filesz, p_memsz = \
            struct.unpack_from('<IIIIII', ph, 0)
        if p_type == 1:                      # PT_LOAD
            dst = base + p_vaddr
            # FIX: the file offset, not the virtual address
            self.mem[self._i(dst):self._i(dst) + p_filesz] = \
                elf[p_off:p_off + p_filesz]
            for j in range(p_filesz, p_memsz):
                self.mem[self._i(dst) + j] = 0
        elif p_type == 2:                    # PT_DYNAMIC
            dyn_off = base + p_vaddr
    rel_idx = relsz = symtab = syment = None
    d = dyn_off
    while True:
        tag, val = struct.unpack_from('<II', self.mem, self._i(d))
        if tag == 17:
            rel_idx = val
        elif tag == 18:
            relsz = val
        elif tag == 6:
            symtab = val
        elif tag == 11:
            syment = val
        elif tag == 0:
            break
        d += 8
    if rel_idx is not None:
        n = relsz // 8
        for i in range(n):
            r_off, r_info = struct.unpack_from('<II', self.mem,
                                               self._i(base + rel_idx + i * 8))
            if (r_info & 0xFF) == 23:        # R_ARM_RELATIVE
                addend = self.rd32(base + r_off)
                self.wr32(base + r_off, (base + addend) & MASK)
            elif (r_info & 0xFF) in (2, 3) and (r_info >> 8) \
                    and symtab is not None:
                st = symtab + (r_info >> 8) * (syment or 16) + 4
                st_value = self.rd32(base + st)
                addend = self.rd32(base + r_off)
                self.wr32(base + r_off,
                          (base + addend + st_value) & MASK)
    return e_entry


apply()
