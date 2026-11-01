#!/usr/bin/env python3
"""make_dist.py -- pack the deployment folder into a release zip, and CHECK IT.

    python3 tools/make_dist.py          (or `make dist`, which runs `make deploy`
                                         first, so the zip is never built from a
                                         stale folder)

WHAT GOES IN
    nbCode-<version>.zip
        nbCode.hpappdir/nbCode.elf        the editor (stripped, ~500 KB)
        nbCode.hpappdir/main.py          the MicroPython launcher
        nbCode.hpappdir/main_notes.md    on-device usage notes
        nbCode.hpappdir/setting.json     the settings the user edits
        nbCode.hpappdir/nbCode.md         the README, as deployed
        INSTALL.txt                     three lines: where to copy it

WHY IT VALIDATES INSTEAD OF JUST ZIPPING
    The one failure that matters is an image the calculator's loader cannot run:
    it loads, the screen stays black, and the only clue is a log ring nobody can
    read.  Every check below is a thing that has actually gone wrong in this
    family of projects (primetcc, primeLua, nbCode):

      * a relocation the loader cannot apply (it applies R_ARM_RELATIVE and
        nothing else, and a symbol reloc leaves a zero where an address belongs);
      * no PT_LOAD segment, or an entry point outside every segment;
      * a missing "NBCORING" ring -- the launcher finds the ring by that magic, so
        without it there is no console and no exit status;
      * an image with debug info left in it, which is 3x the bytes the loader has
        to copy over the debug channel;
      * a launcher whose config format does not match src/nb_cfg.h (the magic and
        the version are read out of the C header here, so the two cannot drift
        silently).

    A check that fails stops the build with the reason, rather than shipping a
    zip that will not boot.
"""
import hashlib
import os
import re
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
DEPLOY = os.path.abspath(os.path.join(TOP, '..', 'nbCode.hpappdir'))
DIST = os.path.join(TOP, 'dist')
APP = 'nbCode.hpappdir'

# the ELF constants that matter (32-bit ARM)
ET_DYN = 3
PT_LOAD = 1
SHT_RELA = 4
SHT_REL = 9
SHT_SYMTAB = 2
SHT_STRTAB = 3
R_ARM_RELATIVE = 23
EM_ARM = 40

# session files: written by the launcher on the calculator, never shipped
SESSION_FILES = {'nbcode.log', 'nbcode.cfg'}

# what the source archive carries (everything but the build products)
SRC_TOP = ['Makefile', 'README.md', 'setting.json']
SRC_DIRS = ['src', 'port', 'host', 'calc', 'tests', 'tools', 'samples']
SKIP_DIRS = {'__pycache__', 'build', 'dist', '.git', 'obj'}

FAILURES = []
NOTES = []


def fail(what):
    FAILURES.append(what)


def read(path, binary=True):
    with open(path, 'rb' if binary else 'r') as f:
        return f.read()


# ---------------------------------------------------------------------------
# the C header is the source of truth for the version and the config format
# ---------------------------------------------------------------------------
def c_defines(header):
    """The #defines of a C header, as a dict (only the plain integer ones)."""
    text = read(os.path.join(TOP, header), binary=False)
    out = {}
    for m in re.finditer(
            r'^#define\s+(\w+)\s+((?:0[xX][0-9a-fA-F]+)|(?:\d+))[uUlL]*(?=\s|$)',
            text, re.M):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def c_string(header, name):
    text = read(os.path.join(TOP, header), binary=False)
    m = re.search(r'#define\s+%s\s+"([^"]*)"' % name, text)
    return m.group(1) if m else None


# ---------------------------------------------------------------------------
# the ELF: the shape the shellcode loader needs, checked the same way the
# launcher checks it on the device (calc/main.py: ElfTools)
# ---------------------------------------------------------------------------
def check_elf(path):
    data = read(path)
    if data[:4] != b'\x7fELF':
        return fail('%s: not an ELF file' % os.path.basename(path))
    if data[4] != 1:                                    # EI_CLASS
        fail('ELF is not 32-bit')
    machine, = struct.unpack_from('<H', data, 18)
    if machine != EM_ARM:
        fail('ELF machine is %d, not ARM (%d)' % (machine, EM_ARM))
    e_type, = struct.unpack_from('<H', data, 16)
    if e_type != ET_DYN:
        # the loader runs the image at an address it chose and fixes it up with
        # R_ARM_RELATIVE: an ET_EXEC image carries absolute addresses that no
        # relocation mentions, so it would jump into the firmware's memory
        fail('e_type is %d, not ET_DYN (%d): the loader needs a position '
             'independent image (build with -fPIC -pie)' % (e_type, ET_DYN))
    entry, phoff, shoff = struct.unpack_from('<III', data, 24)
    phentsize, phnum = struct.unpack_from('<HH', data, 42)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 46)

    segs = []
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from('<8I', data, off)
        if p_type == PT_LOAD:
            segs.append((p_offset, p_vaddr, p_filesz, p_memsz))
    if not segs:
        fail('no PT_LOAD segment: the loader copies segments and would copy '
             'nothing')
    if entry and not any(v <= entry < v + m for (_, v, _, m) in segs):
        fail('entry 0x%X is outside every PT_LOAD segment' % entry)

    # every relocation must be one the loader can apply.  SHT_REL entries are
    # {r_offset, r_info}; SHT_RELA adds an addend, and reading one as the other
    # is how a checker reports types that do not exist (56, 60, ...).
    relocs = 0
    kinds = set()
    symtab = False
    debug = []
    for i in range(shnum):
        off = shoff + i * shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = \
            struct.unpack_from('<6I', data, off)
        if sh_type == SHT_SYMTAB:
            symtab = True
        if sh_type in (SHT_REL, SHT_RELA):
            step = 8 if sh_type == SHT_REL else 12
            for r in range(sh_offset, sh_offset + sh_size, step):
                r_info, = struct.unpack_from('<I', data, r + 4)
                kinds.add(r_info & 0xFF)
                relocs += 1
    # section NAMES, so the notes can say what is in there by name
    str_off = 0
    if shstrndx < shnum:
        str_off, = struct.unpack_from('<I', data,
                                      shoff + shstrndx * shentsize + 16)
    for i in range(shnum):
        off = shoff + i * shentsize
        name_off, sh_type = struct.unpack_from('<II', data, off)
        if str_off:
            end = data.index(b'\0', str_off + name_off)
            name = data[str_off + name_off:end].decode('ascii', 'replace')
            if name.startswith('.debug'):
                debug.append(name)
    bad = sorted(k for k in kinds if k != R_ARM_RELATIVE)
    if bad:
        fail('%d relocation type(s) the loader cannot apply: %s (run '
             'tools/fix_relocs.py via `make`)' % (len(bad), bad))
    NOTES.append('%d relocation(s), all R_ARM_RELATIVE' % relocs)

    if b'NBCORING' not in data:
        fail('no "NBCORING" in the image: the launcher cannot find the log ring, '
             'so there is no console and no exit status')
    if b'NBINPUT' not in data:
        fail('no "NBINPUT" in the image: the input hook state is missing')

    # the loader allocates to the END of the highest segment, not the sum of the
    # sizes: the segments are 64 KB-aligned apart, so the gap between them is
    # real firmware heap
    return data, {'relocs': relocs, 'symtab': symtab, 'debug': debug,
                  'entry': entry,
                  'load_bytes': sum(f for (_, _, f, _) in segs),
                  'alloc_bytes': max(v + m for (_, v, _, m) in segs),
                  'zero_bytes': sum(m - f for (_, _, f, m) in segs)}


def check_launcher():
    """The launcher's own constants must match src/nb_cfg.h."""
    src = read(os.path.join(TOP, 'calc', 'main.py'), binary=False)
    hdr = c_defines('src/nb_cfg.h')
    want_magic = hdr.get('NB_CFG_MAGIC')
    want_version = hdr.get('NB_CFG_VERSION')
    m = re.search(r'^CFG_MAGIC\s*=\s*(0x[0-9a-fA-F]+|\d+)', src, re.M)
    v = re.search(r'^CFG_VERSION\s*=\s*(\d+)', src, re.M)
    if not m or not v:
        return fail('calc/main.py has no CFG_MAGIC/CFG_VERSION')
    if int(m.group(1), 0) != want_magic:
        fail('launch-file magic disagrees: main.py %s, src/nb_cfg.h 0x%08X'
             % (m.group(1), want_magic))
    if int(v.group(1)) != want_version:
        fail('launch-file version disagrees: main.py %s, src/nb_cfg.h %d'
             % (v.group(1), want_version))
    name = re.search(r"^CFG_FILE\s*=\s*'([^']+)'", src, re.M)
    if not name:
        fail('calc/main.py does not name the launch file')
    elif name.group(1) != c_string('src/nb_cfg.h', 'NB_CFG_FILE_NAME'):
        fail('launch-file name disagrees: main.py %r' % name.group(1))
    NOTES.append('launcher and src/nb_cfg.h agree (magic 0x%08X, version %d)'
                 % (want_magic, want_version))


DQ = '"' * 3
SQ = "'" * 3


def check_deployed_launcher():
    """The launcher that SHIPS must be the stripped one, and it must compile.

    The calculator compiles an imported module's source into its own memory, so
    `make deploy` runs tools/strip_prose.py over it (see that file).  Shipping the
    commented source would cost megabytes of RAM on a 32 MB machine -- invisible
    from here, and visible on the device only as "not enough memory".
    """
    shipped = os.path.join(DEPLOY, 'main.py')
    source = os.path.join(TOP, 'calc', 'main.py')
    if not os.path.exists(shipped):
        return fail('main.py is missing from the deployment folder')
    text = read(shipped, binary=False)
    try:
        compile(text, 'main.py', 'exec')
    except SyntaxError as e:
        return fail('the deployed main.py does not parse: %s' % (e,))
    here, there = len(text), os.path.getsize(source)
    if here >= there * 0.9:
        fail('the deployed main.py looks UNSTRIPPED (%d B of %d B): `make '
             'deploy` runs tools/strip_prose.py over it' % (here, there))
    comments = [ln for ln in text.split('\n') if ln.lstrip().startswith('#')]
    if comments:
        fail('the deployed main.py still has %d comment line(s)' % len(comments))
    if text.lstrip()[:3] not in (DQ, SQ):
        fail('the deployed main.py has no module docstring: help(main) would be '
             'empty on the calculator')
    NOTES.append('main.py: %d B deployed of %d B commented (%.0f%%)'
                 % (here, there, 100.0 * here / there))


# ---------------------------------------------------------------------------
# INSTALL.txt: the whole deployment story in a few lines
# ---------------------------------------------------------------------------
INSTALL = """\
nbCode {version} -- a code editor for the HP Prime G1

COPY THIS FOLDER TO THE CALCULATOR:
    nbCode.hpappdir  ->  C:\\DATA\\nbCode.hpappdir\\

(The Connectivity Kit copies folders into C:\\DATA; the folder name must keep
the .hpappdir suffix.)

RUN IT:
    On the calculator:  Applications -> nbCode -> main.py
    Or from the MicroPython app:
        import main
        main.run()

    The start screen lists workspaces; ENTER opens one, ENTER on a file opens it
    in the editor.  ON quits.  The full key map is in nbCode.md.

SETTINGS:  nbCode.hpappdir\\setting.json (theme, tab_width, language, workspaces)
LOG:       nbCode.hpappdir\\nbcode.log is written on every run; if the editor ever
           fails to start, that file says what it got to.

Image: {elf_bytes} bytes, sha256 {elf_sha}
"""


def main():
    if not os.path.isdir(DEPLOY):
        print('make_dist: %s does not exist -- run `make deploy` first' % DEPLOY)
        return 1

    elf = os.path.join(DEPLOY, 'nbCode.elf')
    for needed in ('nbCode.elf', 'main.py', 'setting.json'):
        if not os.path.exists(os.path.join(DEPLOY, needed)):
            fail('%s is missing from the deployment folder' % needed)
    if FAILURES:
        return report()

    got = check_elf(elf)
    if not got:
        return report()
    data, facts = got
    check_launcher()
    check_deployed_launcher()
    NOTES.append('entry 0x%X: %d B copied, %d B of firmware heap (%.0f KB), of '
                 'which %d B is zeroed .bss'
                 % (facts['entry'], facts['load_bytes'], facts['alloc_bytes'],
                    facts['alloc_bytes'] / 1024.0, facts['zero_bytes']))

    # debug info and symbols: the loader never reads either, and on the device
    # every byte of the image is debug-channel traffic while it is copied in.
    # (nbCode's device build carries no -g, so `--strip-debug` has little to do;
    # the symbol table is what is left, and primeLua ships it too.)
    if facts['debug']:
        NOTES.append('%d debug section(s) still in the image (%s): `make deploy` '
                     'strips them' % (len(facts['debug']),
                                      ', '.join(facts['debug'][:3])))
    if facts['symtab']:
        NOTES.append('%d B outside the segments (ELF header, program headers and '
                     'the symbol table): the loader reads the headers and skips '
                     'the rest; the symbols stay because they are what makes a '
                     'crash dump readable'
                     % (os.path.getsize(elf) - facts['load_bytes']))

    if FAILURES:
        return report()

    version = c_string('src/nb_port.h', 'NB_VERSION') or '0.0'
    if len(data) > 600 * 1024:
        NOTES.append('the image is %d B and the loader has been proven to about '
                     '520 KB: check the free heap after loading' % len(data))

    os.makedirs(DIST, exist_ok=True)
    # a dist/ folder holds this run's archives and nothing else: an old zip under
    # a name this version no longer produces is how a stale build gets shipped
    for name in os.listdir(DIST):
        if name.endswith('.zip'):
            os.remove(os.path.join(DIST, name))
    appzip = os.path.join(DIST, 'nbCode.hpappdir.zip')
    srczip = os.path.join(DIST, 'nbCode-src.zip')

    # (1) the app folder, exactly as it must end up in C:\DATA\ -- the folder
    # name inside the archive is the name the calculator sees
    with zipfile.ZipFile(appzip, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in sorted(os.listdir(DEPLOY)):
            p = os.path.join(DEPLOY, name)
            if not os.path.isfile(p) or name in SESSION_FILES:
                continue
            z.write(p, '%s/%s' % (APP, name))
        z.writestr('INSTALL.txt', INSTALL.format(
            version=version, elf_bytes=len(data),
            elf_sha=hashlib.sha256(data).hexdigest()[:16]))

    # (2) the sources, so the thing can be rebuilt: nbCode has no upstream tree,
    # so this is everything except the build products (and the vendored emulator
    # is IN, because `make test-emu` is part of rebuilding it)
    with zipfile.ZipFile(srczip, 'w', zipfile.ZIP_DEFLATED) as z:
        for entry in SRC_TOP:
            path = os.path.join(TOP, entry)
            if os.path.isfile(path):
                z.write(path, 'nbCode/' + entry)
        for tree in SRC_DIRS:
            root = os.path.join(TOP, tree)
            for dirpath, dirnames, filenames in os.walk(root):
                dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
                for name in sorted(filenames):
                    path = os.path.join(dirpath, name)
                    if name.endswith('.pyc') or name in SESSION_FILES:
                        continue
                    rel = os.path.relpath(path, TOP)
                    z.write(path, 'nbCode/' + rel)

    print('==> %s (%d B)' % (appzip, os.path.getsize(appzip)))
    with zipfile.ZipFile(appzip) as z:
        for info in z.infolist():
            print('    %-34s %8d B' % (info.filename, info.file_size))
    print('==> %s (%d B)' % (srczip, os.path.getsize(srczip)))
    for n in NOTES:
        print('    - %s' % n)
    return 0


def report():
    print('make_dist: REFUSING TO PACKAGE:')
    for f in FAILURES:
        print('    ! %s' % f)
    return 1


if __name__ == '__main__':
    sys.exit(main())
