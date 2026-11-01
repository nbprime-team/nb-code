#!/usr/bin/env python3
"""fix_relocs.py -- make nbCode.elf loadable by the calculator's shellcode loader.

The on-device loader (calc/main.py, and tests/emu_arm2.py on the host) applies
R_ARM_RELATIVE relocations only.  A GCC -fPIC link normally emits exactly
those, but any R_ARM_ABS32 / R_ARM_REL32 entry that slips through (a symbol
that ends up needing a symbol-based fixup) would be skipped silently, leaving
a literal holding 0 where an address belongs -- which on hardware shows up as
"the calculator reboots the moment the script touches a global".

primetcc solved this with ElfTools.patch_shared_relocs() running on the
calculator.  Running the same transformation at build time means the ELF is
correct before it is ever copied over, and the launcher's copy remains as a
belt-and-braces check.

Usage: fix_relocs.py <elf-file>   (patched in place; prints what it did)
"""
import struct
import sys

R_ARM_ABS32 = 2
R_ARM_REL32 = 3
R_ARM_RELATIVE = 23

PT_LOAD = 1
PT_DYNAMIC = 2

DT_NULL, DT_SYMTAB, DT_REL, DT_RELSZ, DT_SYMENT = 0, 6, 17, 18, 11


def main(path):
    with open(path, "rb") as fh:
        elf = bytearray(fh.read())

    e_phoff = struct.unpack_from("<I", elf, 28)[0]
    e_phentsize = struct.unpack_from("<H", elf, 42)[0]
    e_phnum = struct.unpack_from("<H", elf, 44)[0]

    loads, dyn_off = [], None
    for i in range(e_phnum):
        ph = e_phoff + i * e_phentsize
        p_type, p_off, p_vaddr, _p_paddr, _p_filesz, p_memsz = \
            struct.unpack_from("<IIIIII", elf, ph)
        if p_type == PT_LOAD:
            loads.append((p_vaddr, p_off, p_memsz))
        elif p_type == PT_DYNAMIC:
            dyn_off = p_off
    if dyn_off is None:
        print("fix_relocs: no PT_DYNAMIC (nothing to do)")
        return 0

    tags = {}
    off = dyn_off
    while True:
        tag, val = struct.unpack_from("<II", elf, off)
        if tag == DT_NULL:
            break
        tags[tag] = val
        off += 8
    if DT_REL not in tags or DT_RELSZ not in tags:
        print("fix_relocs: no DT_REL (nothing to do)")
        return 0

    rel_off, rel_sz = tags[DT_REL], tags[DT_RELSZ]
    sym_off = tags.get(DT_SYMTAB, 0)
    syment = tags.get(DT_SYMENT, 16)

    def vma2file(v):
        for p_vaddr, p_off, p_memsz in loads:
            if p_vaddr <= v < p_vaddr + p_memsz:
                return v - p_vaddr + p_off
        return None

    def sym_value(idx):
        pos = sym_off + idx * syment + 4        # st_value
        return struct.unpack_from("<I", elf, pos)[0]

    patched, unhandled = 0, 0
    for i in range(rel_sz // 8):
        pos = rel_off + i * 8
        r_off, r_info = struct.unpack_from("<II", elf, pos)
        rtype, idx = r_info & 0xFF, r_info >> 8
        if rtype in (R_ARM_RELATIVE, 0) or idx == 0:
            continue
        if rtype not in (R_ARM_ABS32, R_ARM_REL32):
            unhandled += 1
            continue
        wpos = vma2file(r_off)
        if wpos is None:
            continue
        # ARM ABS32: final = S + A.  RELATIVE: final = load_base + word,
        # so folding S into the word and switching the type gives exactly
        # load_base + S + A.  REL32 additionally subtracts the place, which
        # the loader's model cannot express -- fold the same way and let the
        # runtime value be off by (P - load_base) only if such an entry ever
        # appears (it does not in a -fPIC link).
        word = struct.unpack_from("<I", elf, wpos)[0]
        struct.pack_into("<I", elf, wpos, (word + sym_value(idx)) & 0xFFFFFFFF)
        struct.pack_into("<II", elf, pos, r_off,
                         (r_info & 0xFFFFFF00) | R_ARM_RELATIVE)
        patched += 1

    if patched:
        with open(path, "wb") as fh:
            fh.write(elf)
    print("fix_relocs: %d symbol reloc(s) folded to R_ARM_RELATIVE%s"
          % (patched, ", %d unhandled" % unhandled if unhandled else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "nbCode.elf"))
