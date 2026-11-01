"""
main.py -- nbCode's launcher: the Python half of a C code editor on the HP Prime.

WHAT THIS FILE IS FOR
    The calculator's MicroPython application is the only place with a directory
    enumeration, a JSON parser and a way to start native code.  So the launcher:

      1. finds its own folder and reads nbCode.hpappdir\\setting.json;
      2. lists the workspaces and the files in the selected one;
      3. finds and checks nbCode.elf;
      4. loads it with the shellcode ELF loader DOOM, primetcc and primeLua use
         (malloc an image block, copy the PT_LOAD segments, apply the
         R_ARM_RELATIVE relocations);
      5. builds ONE flat launch file, nbcode.cfg (src/nb_cfg.h), holding the
         settings, the workspaces, the file list and the paths, writes it next
         to the image, and calls the ELF entry;
      6. reads the image's log ring when it returns -- the run is ONE debug
         call, and the ring is where the image leaves its console output and its
         exit status, so a session that dies still says what it was doing;
      7. when nbCode returns, parses "RT_RET:<n>" out of the ring, disarms the
         input hook if the image died without disarming it, and prints the
         result.

WHY THE EDITOR IS IN C AND NOT PYTHON
    The Python PrimeCode in this workspace works, and its file manager idiom is
    what nbCode's start screen copies.  What it cannot do is draw a 320x240 screen
    of 8x16 bitmap glyphs 30 times a second: MicroPython on this CPU spends
    ~100 ms redrawing a screenful of text.  The C side draws the same screen in
    2 ms, which is what makes a 40x13 editor with syntax colouring usable.

ON-DEVICE USAGE
    Applications -> nbCode -> main.py runs this file, and so does `import main`
    on the calculator: both start the same menu (see the bottom of the file --
    the entry point is the interpreter, not __name__).

    main.run()                  -> run with the settings as they are
    main.run("C:\\\\DATA\\\\nbCode.hpappdir\\\\hello.c")   -> open one file
    main.probe()                -> the load chain, step by step, no editor
    main.unload()               -> give the image's memory back
    main.SELFTEST_NO_EDITOR = True   -> load and exit: for bring-up on hardware

WHAT THE CALCULATOR DOES NOT HAVE is written down in README.md (section 6.1) and
in main_notes.md: no `struct` (it is `ustruct`), no `time`, no `json`, no
absolute paths in its file API, and no useful `os`.  Each one of those was a
failure on hardware before it was a rule here.
"""
import sys

# NO `struct` AND NO `time` ON THE CALCULATOR.  The firmware's MicroPython calls
# the module `ustruct`, and there is no time module at all (the C side has the
# firmware's clock; the launcher's pacing is a frame count, not a delay).  An
# earlier version imported both by their desktop names and did not start.
try:
    import ustruct as struct                       # the calculator
except ImportError:                                # a host running CPython
    import struct

# uio is the calculator's file module.  It is used through open_file() below
# rather than directly, because the HOST has no uio and because a name that
# cannot be opened has to fall through to the other spelling.
try:
    import uio as _uio
except ImportError:
    _uio = None

try:
    import ujson as json
except ImportError:                                # pragma: no cover
    try:
        import json
    except ImportError:
        json = None

try:
    import hpprime
except ImportError:                                # host tests import this
    hpprime = None

# ---------------------------------------------------------------------------
# THE CONSOLE SPEAKS `moreprint` WHEN IT IS THERE
#
# primeLua and primetcc both print through helpers from a `moreprint` module the
# user drops next to main.py, so ONE file decides how every launcher's console
# output looks (its colours, on a machine whose console is a 320x240 screen).
# nbCode takes the same four and gives them the same meanings:
#
#     p_log      what the launcher is doing          [load] ..., [cost] ..., progress
#     p_warning  something worth a look: a fallback  "calloc refused, malloc"
#     p_error    something failed                    "[load] FAIL: ..."
#     p_pass     a step that WORKED                  "the editor came back"
#
# p_out -- a program's own standard output -- is the one nbCode does not take: it
# has no program output, because the editor owns the screen from the moment it
# starts, and what the C side has to say goes to nbcode.log instead.
#
# WITHOUT THE MODULE all four fall back to print(), so the console reads exactly
# as it did before and a calculator without moreprint.py loses only the colour.
# ---------------------------------------------------------------------------
try:
    from moreprint import p_log, p_warning, p_error, p_pass
except Exception:
    def p_log(text):
        print(text)

    def p_warning(text):
        print(text)

    def p_error(text):
        print(text)

    def p_pass(text):
        print(text)


def open_file(name, mode='rb'):
    """Open a file THE WAY THIS MACHINE CAN.

    BARE NAMES ONLY.  MicroPython resolves a relative name against the
    application folder -- which is exactly where nbCode's files are -- and on this
    firmware it REFUSES an absolute "C:\\DATA\\..." path outright (OSError 0).
    Every open in this file therefore asks for a bare name first, and the
    candidate list in read_bytes() is what makes the same code work on a host.
    """
    if _uio is not None:
        try:
            return _uio.FileIO(name, mode)
        except Exception:
            pass                                       # fall through to open()
    return open(name, mode)


class _IOModule:
    """`io.FileIO` for the rest of this file, backed by uio where there is one.

    (A shim rather than a direct import: `import uio as io` fails on the host,
    and a name that does not open has to be retried the other way.)"""

    FileIO = staticmethod(open_file)


io = _IOModule


# WHERE THE INPUT STATE'S FLIGHT COUNTER LIVES (nb_input.h: struct nb_in_state).
# The launcher's contract with that struct is these four fields: the saved slot
# bytes at 12, saved_valid at 28, armed at 32, hook_busy at 36.
HOOK_BUSY_OFF = 36

# blocks kept because a hook call was still inside them when the run ended (see
# ShellcodeElfLoader.unload): freed before the next load, once the OS has left
_held = []


# ---------------------------------------------------------------------------
# the debug interface: three firmware SVCs, addressed through their stubs
#
# The firmware's service table starts at 0x307FBCAC with 12 bytes per service,
# and the debug device ("debug") is how a Python application reads and writes
# another process's memory: DOOM and primeLua both drive their loaded images
# through it, and this is the same three-call protocol (see REVERSE_README.md).
# ---------------------------------------------------------------------------
BASE = 0x307FBCAC


def get_addr(svc):
    return BASE + 0xC * (svc - 0x10000)


ADDR_MALLOC = get_addr(0x10037)
ADDR_CALLOC = get_addr(0x10038)     # malloc's brother: the block comes ZEROED
ADDR_FREE = get_addr(0x1003A)

# The firmware's own file services, reached the same way.  The launcher needs
# them because MicroPython can only see the application folder: listing a folder
# elsewhere on the disk, and asking where the application folder IS, are both
# things only the firmware can do.  (primeLua's launcher does exactly this.)
FW_FIND_FIRST = get_addr(0x10270)
FW_FIND_NEXT = get_addr(0x10271)
FW_FIND_CLOSE = get_addr(0x10273)
FW_GET_ATTR = get_addr(0x10272)
FW_GET_DISK_ID = get_addr(0x100E7)
FW_GETCWD = get_addr(0x1027A)
FW_FOPEN = get_addr(0x1026F)
FW_FCLOSE = get_addr(0x100CA)
FW_FSEEK = get_addr(0x100CF)
FW_FREAD = get_addr(0x100D4)

# the firmware's get_event slot: the input hook is installed THERE, by the image
FW_EVENT_SLOT = 0x307FBFA0
SLOT_TRAP = 0xE51FF004


def is_device():
    """True on the calculator, False on a host running CPython.

    NOT `__name__ == '__main__'`.  The calculator's Python application does not
    run a file the way CPython does (and `import main` has to work as well), so
    the entry point at the bottom of this file keys off the interpreter instead:
    `sys.implementation.name` is "micropython" there and "cpython" here.
    """
    try:
        impl = getattr(sys, 'implementation', None)
        name = getattr(impl, 'name', '') if impl is not None else ''
        if name:
            return name == 'micropython'
    except Exception:
        pass
    # no sys.implementation at all: the calculator's Python application is the
    # only environment in this family that has hpprime, so that is the next best
    # signal (it is what primeLua's launcher keys off)
    return hpprime is not None


def str_to_utf16le_bytes(s):
    """UTF-16LE with a terminator, built by hand: the firmware's wide strings,
    and the encoding a MicroPython codec cannot always be trusted to provide."""
    out = bytearray()
    for ch in s:
        v = ord(ch)
        out.append(v & 0xFF)
        out.append((v >> 8) & 0xFF)
    out.append(0)
    out.append(0)
    return bytes(out)


def decode_bytes(raw):
    """bytes -> str, and it NEVER raises.

    THE DEVICE'S CODEC IS NOT CPYTHON'S.  MicroPython does not implement the
    non-strict error handlers: `b'\\xff'.decode('utf-8', 'replace')` is a
    UnicodeError there, whatever the data is -- and the launcher reads memory
    that is full of bytes nobody validated (the image's log ring, whose 64 byte
    tail can start in the middle of a Chinese character, is the one that bit).
    So the rule in this file is: pass no `errors=` argument to anything, and
    have a fallback that is plain Python.  tests/test_emu.py enforces both (the
    fake device raises like the real one, and an AST check refuses a non-strict
    handler in the deployed source)."""
    try:
        return raw.decode('utf-8')
    except Exception:
        pass
    try:
        return raw.decode('latin-1')
    except Exception:
        pass
    return ''.join(chr(b) for b in raw)


def utf8_bytes(s):
    """str -> UTF-8 bytes, built by hand, and it never raises either.

    Everything the launcher WRITES to the device goes through here: the launch
    file's strings and the log.  `s.encode('utf-8', 'replace')` is the obvious
    spelling and the wrong one on this machine (see decode_bytes), and a raise
    here costs the whole session because it happens while the launch file is
    being written."""
    out = bytearray()
    for ch in s:
        v = ord(ch)
        if v < 0x80:
            out.append(v)
        elif v < 0x800:
            out.append(0xC0 | (v >> 6))
            out.append(0x80 | (v & 0x3F))
        elif 0xD800 <= v < 0xE000:
            out.append(0x3F)		# a lone surrogate is not a character
        elif v < 0x10000:
            out.append(0xE0 | (v >> 12))
            out.append(0x80 | ((v >> 6) & 0x3F))
            out.append(0x80 | (v & 0x3F))
        elif v < 0x110000:
            out.append(0xF0 | (v >> 18))
            out.append(0x80 | ((v >> 12) & 0x3F))
            out.append(0x80 | ((v >> 6) & 0x3F))
            out.append(0x80 | (v & 0x3F))
        else:
            out.append(0x3F)		# '?' -- a codepoint UTF-8 cannot carry
    return bytes(out)


class MemUtils:
    """mallocs in the firmware heap that the firmware calls below need.

    A firmware call takes POINTERS: the file finder wants a UTF-16 path and a
    256-byte context block, and the answers come back as pointers to strings
    that have to be read out of memory and freed.  This is the small piece of
    glue that makes that possible from Python (primeLua's MemUtils is the same
    thing).
    """

    def __init__(self, dbg):
        self.dbg = dbg

    def malloc(self, size):
        return self.dbg.call(ADDR_MALLOC, (size + 3) & ~3)

    def free(self, addr):
        if addr:
            self.dbg.call(ADDR_FREE, addr)

    def alloc_wstr(self, s):
        b = str_to_utf16le_bytes(s)
        addr = self.malloc(len(b))
        if addr:
            self.dbg.write_mem_bytes(addr, b)
        return addr

    def read_wstr(self, addr, max_chars=260):
        if not addr:
            return ''
        raw = self.dbg.read_mem(addr, max_chars * 2)
        out = []
        for i in range(0, len(raw) - 1, 2):
            c = raw[i] | (raw[i + 1] << 8)
            if c == 0:
                break
            out.append(chr(c) if 32 <= c < 0xFFFD else '?')
        return ''.join(out)

    def read_utf8(self, addr, max_len=260):
        if not addr:
            return ''
        raw = self.dbg.read_mem(addr, max_len)
        end = raw.find(b'\x00')
        if end >= 0:
            raw = raw[:end]
        return decode_bytes(bytes(raw))



def open_debug():
    """The firmware's "debug" device, opened the way the OTHER launchers here
    open it: uio without a mode first, then plain open().

    Copied from primetcc/primeLua deliberately.  This one line has no margin for
    invention: everything else in the launcher goes through it, and "r+b" (the
    obvious spelling) is NOT what the two projects that run on this hardware
    use.
    """
    if _uio is not None:
        try:
            return _uio.FileIO('debug')
        except Exception:
            pass
    return open('debug', 'rb')


class PrimeDebug:
    """read_mem / write_mem / call, over the firmware's "debug" device.
    The device is a FILE with a tiny protocol (REVERSE_README.md): write
    {op, addr, value} triples and read the answers back.  primetcc's launcher
    and DOOM's both drive their images this way, and it is the only route that
    works from a loaded image's perspective -- PPL's eval() would work from
    Python but is not what any other tool here uses, and it cannot be tested on
    the host.

        0  read_mem(addr, n)   -> write (0, addr, 0), read n bytes
        1  write_mem(addr, v)  -> write (1, addr, v)
        2  call(fn, a0..a3)    -> write (2, fn, count, args...), read the answer
    """

    def __init__(self):
        self.f = None
        # EVERY round trip to the firmware, counted: it is the cost that makes a
        # start-up slow on this machine (one device write plus one read each), and
        # there is no clock to measure it with
        self.round_trips = 0

    def open(self):
        if self.f is None:
            self.f = open_debug()
        return self.f

    # NO seek(), NO flush() -- the two launchers that run on this hardware
    # (primetcc's and primeLua's) do neither, and "the debug device is a file" is
    # true only up to a point: a write is a COMMAND and a read is its ANSWER, and
    # the device does not model a file position at all.
    def read_mem(self, addr, n):
        f = self.open()
        self.round_trips += 1
        f.write(struct.pack('<III', 0, addr, 0))
        return f.read(n)

    def write_mem(self, addr, value):
        f = self.open()
        self.round_trips += 1
        f.write(struct.pack('<III', 1, addr, value & 0xFFFFFFFF))

    def write_mem_bytes(self, addr, data):
        data = bytes(data)
        pad = (4 - (len(data) % 4)) % 4
        data += b'\x00' * pad
        for i in range(0, len(data), 4):
            self.write_mem(addr + i, struct.unpack('<I', data[i:i + 4])[0])

    def call(self, addr, r0=0, r1=0, r2=0, r3=0):
        """Call `addr` and return its r0.

        THE ANSWER COMES BACK IN THE BUFFER WE JUST WROTE: the debug device
        overwrites the command block with the callee's result, which is why this
        writes a bytearray and then unpacks from it rather than reading anything
        back.  That is DOOM's mechanism, and primetcc and primeLua both use it
        unchanged.
        """
        f = self.open()
        self.round_trips += 1
        args = [r0, r1, r2, r3]
        buf = bytearray(struct.pack('<IIIIII', 2, addr, len(args), *args))
        f.write(buf)
        return struct.unpack_from('<I', buf, 0)[0]

    def close(self):
        if self.f is not None:
            self.f.close()
            self.f = None


# ---------------------------------------------------------------------------
# ELF: what the loader needs to know about an image
# ---------------------------------------------------------------------------
class ElfTools:
    @staticmethod
    def seg_end(data):
        """the highest byte any PT_LOAD segment occupies, base relative.

        This is where the image's memory ENDS, which is not the same as the last
        segment's own end: a two segment image (text then .data, page aligned)
        leaves a hole between them, and writing anything into that hole means
        writing into a segment's .bss -- which the launcher clears, on purpose,
        during the load.  The launcher puts the launch config AFTER this value
        (see run_file) so the two can never overlap.
        """
        if len(data) < 52 or data[0:4] != b'\x7fELF':
            return 0
        phoff = struct.unpack_from('<I', data, 28)[0]
        phent = struct.unpack_from('<H', data, 42)[0]
        phnum = struct.unpack_from('<H', data, 44)[0]
        need = 0
        for i in range(phnum):
            off = phoff + i * phent
            if off + 32 > len(data):
                break
            if struct.unpack_from('<I', data, off)[0] != 1:   # PT_LOAD
                continue
            vaddr = struct.unpack_from('<I', data, off + 8)[0]
            memsz = struct.unpack_from('<I', data, off + 20)[0]
            if vaddr + memsz > need:
                need = vaddr + memsz
        return need

    @staticmethod
    def memory_size(data):
        """bytes the image needs: the end above, rounded up."""
        return (ElfTools.seg_end(data) + 3) & ~3

    @staticmethod
    def entry_point(data):
        if len(data) < 52 or data[0:4] != b'\x7fELF':
            return 0
        return struct.unpack_from('<I', data, 24)[0]

    @staticmethod
    def find_magic(data, magic, base):
        """the ADDRESS of `magic` once the image is loaded at `base`."""
        if len(data) < 52 or data[0:4] != b'\x7fELF':
            return 0
        phoff = struct.unpack_from('<I', data, 28)[0]
        phent = struct.unpack_from('<H', data, 42)[0]
        phnum = struct.unpack_from('<H', data, 44)[0]
        segs = []
        for i in range(phnum):
            off = phoff + i * phent
            if off + 32 > len(data):
                break
            if struct.unpack_from('<I', data, off)[0] == 1:
                segs.append((struct.unpack_from('<I', data, off + 4)[0],
                             struct.unpack_from('<I', data, off + 8)[0],
                             struct.unpack_from('<I', data, off + 16)[0]))
        idx = data.find(magic)
        if idx < 0:
            return 0
        for (p_off, p_vaddr, p_filesz) in segs:
            if p_off <= idx < p_off + p_filesz:
                return base + p_vaddr + (idx - p_off)
        return 0

    @staticmethod
    def segments(data):
        """[(file offset, vaddr, filesz, memsz)] for every PT_LOAD."""
        out = []
        phoff = struct.unpack_from('<I', data, 28)[0]
        phent = struct.unpack_from('<H', data, 42)[0]
        phnum = struct.unpack_from('<H', data, 44)[0]
        for i in range(phnum):
            off = phoff + i * phent
            if off + 32 > len(data):
                break
            fields = struct.unpack_from('<IIIIIIII', data, off)
            if fields[0] != 1:
                continue
            out.append((fields[1], fields[2], fields[4], fields[5]))
        return out

    @staticmethod
    def relocations(data):
        """[(offset, type, symbol value)] for the entries the loader must apply.

        ONLY R_ARM_RELATIVE IS EVER APPLIED BY THE LOADER in the firmware, so the
        build folds anything else into a RELATIVE entry at build time
        (tools/fix_relocs.py); this returns the list as it stands and the caller
        refuses an image that still has symbol-based entries.
        """
        if len(data) < 52:
            return [], 0
        phoff = struct.unpack_from('<I', data, 28)[0]
        phent = struct.unpack_from('<H', data, 42)[0]
        phnum = struct.unpack_from('<H', data, 44)[0]
        dyn = None
        for i in range(phnum):
            off = phoff + i * phent
            if off + 32 > len(data):
                break
            if struct.unpack_from('<I', data, off)[0] == 2:      # PT_DYNAMIC
                dyn = struct.unpack_from('<I', data, off + 4)[0]
        if dyn is None:
            return [], 0
        tags = {}
        off = dyn
        while off + 8 <= len(data):
            tag, val = struct.unpack_from('<II', data, off)
            if tag == 0:
                break
            tags[tag] = val
            off += 8
        rel, relsz = tags.get(17), tags.get(18)
        if not rel or not relsz:
            return [], 0
        out, unknown = [], 0
        for i in range(relsz // 8):
            off = rel + i * 8
            r_off, r_info = struct.unpack_from('<II', data, off)
            rtype = r_info & 0xFF
            if rtype == 23 or (r_info >> 8) == 0:                # RELATIVE
                out.append((r_off, 23, 0))
            else:
                unknown += 1
        return out, unknown


# ---------------------------------------------------------------------------
# the loader: this is the part the calculator actually requires, and it is the
# one DOOM and primetcc have been running on this hardware for years
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# THE IN-FIRMWARE LOADER, taken from primeLua (calc/main.py: LOADER_CODE)
#
# WHY THIS EXISTS AT ALL: copying an image from Python costs ONE DEBUG ROUND TRIP
# PER FOUR BYTES.  A 476 KB image is therefore 121,875 round trips, which took
# minutes on the calculator -- measured, in this launcher's own log
# ("[cost] image load / reuse 121875 round trip(s)").  The way out is not a faster
# loop, it is not doing the copying here: this blob is uploaded into the firmware
# heap and does the whole load IN ARM CODE -- fopen, read the ELF header, copy each
# PT_LOAD segment, apply the R_ARM_RELATIVE relocations -- and returns the entry
# address.  One call.  It is the loader primeLua has been running on this hardware
# all along, which is why it is copied rather than written.
#
# Interface (see load_via_blob): call(blob, path16, image_base) -> entry, where
# path16 is a NUL-terminated UTF-16LE device path in firmware memory.
# ---------------------------------------------------------------------------
LOADER_CODE = (
    b'\x0b\x00\x00\xea\x04\x00\x2d\xe5\x04\xe0\x2d\xe5\x6f\x02\x01\xef'
    b'\x04\x00\x2d\xe5\x04\xe0\x2d\xe5\xca\x00\x01\xef\x04\x00\x2d\xe5'
    b'\x04\xe0\x2d\xe5\xd4\x00\x01\xef\x04\x00\x2d\xe5\x04\xe0\x2d\xe5'
    b'\xcf\x00\x01\xef\x68\x32\x9f\xe5\xf0\x4f\x2d\xe9\x03\x30\x8f\xe0'
    b'\x00\x20\xa0\xe1\x01\x60\xa0\xe1\x03\x00\x93\xe8\x64\xd0\x4d\xe2'
    b'\x04\x00\x8d\xe5\xb8\x10\xcd\xe1\x02\x00\xa0\xe1\x04\x10\x8d\xe2'
    b'\xe7\xff\xff\xeb\x00\x40\x50\xe2\x73\x00\x00\x0a\x04\x30\xa0\xe1'
    b'\x01\x20\xa0\xe3\x34\x10\xa0\xe3\x2c\x00\x8d\xe2\xe6\xff\xff\xeb'
    b'\x01\x00\x50\xe3\x6a\x00\x00\x1a\xbc\x22\xdd\xe1\x14\x32\x9f\xe5'
    b'\x03\x00\x52\xe1\x66\x00\x00\x1a\x48\x10\x9d\xe5\x00\x20\xa0\xe3'
    b'\x04\x00\xa0\xe1\xdf\xff\xff\xeb\xb8\x35\xdd\xe1\x00\x00\x53\xe3'
    b'\x78\x00\x00\x0a\x00\x50\xa0\xe3\x05\x70\xa0\xe1\x0c\xa0\x8d\xe2'
    b'\x04\x00\x00\xea\xb8\x35\xdd\xe1\x02\x00\x5b\xe3\x14\x70\x9d\x05'
    b'\x05\x00\x53\xe1\x19\x00\x00\xda\x04\x30\xa0\xe1\x01\x20\xa0\xe3'
    b'\x20\x10\xa0\xe3\x0a\x00\xa0\xe1\xcb\xff\xff\xeb\x0c\xb0\x9d\xe5'
    b'\x01\x50\x85\xe2\x01\x00\x5b\xe3\xf1\xff\xff\x1a\x1c\x30\x9d\xe5'
    b'\x14\x80\x9d\xe5\x48\x90\x9d\xe5\x00\x00\x53\xe3\x08\x80\x86\xe0'
    b'\x85\x92\x89\xe0\x4b\x00\x00\x1a\x20\x20\x9d\xe5\x03\x00\x52\xe1'
    b'\x55\x00\x00\x8a\x09\x10\xa0\xe1\x00\x20\xa0\xe3\x04\x00\xa0\xe1'
    b'\xbc\xff\xff\xeb\xb8\x35\xdd\xe1\x05\x00\x53\xe1\xe5\xff\xff\xca'
    b'\x04\x00\xa0\xe1\xb1\xff\xff\xeb\x00\x00\x57\xe3\x2f\x00\x00\x0a'
    b'\x07\x30\x96\xe7\x07\x70\x86\xe0\x00\x00\x53\xe3\x2b\x00\x00\x0a'
    b'\x00\x20\xa0\xe3\x08\xc0\xa0\xe3\x02\x00\xa0\xe1\x02\xe0\xa0\xe1'
    b'\x11\x00\x53\xe3\x04\xe0\x97\x05\x04\x00\x00\x0a\x12\x00\x53\xe3'
    b'\x04\x00\x97\x05\x01\x00\x00\x0a\x13\x00\x53\xe3\x04\xc0\x97\x05'
    b'\x08\x30\xb7\xe5\x01\x20\x82\xe2\x00\x00\x53\xe3\x64\x00\x52\x13'
    b'\x01\x10\xa0\x13\x00\x10\xa0\x03\xf0\xff\xff\x1a\x00\x00\x5e\xe3'
    b'\x00\x00\x50\x13\x15\x00\x00\x0a\x00\x00\x5c\xe3\x08\xc0\xa0\x03'
    b'\x0c\x00\x50\xe1\x11\x00\x00\x3a\x0c\x00\x40\xe0\x00\x00\x5c\xe1'
    b'\x01\x10\x81\xe2\xfb\xff\xff\x9a\x00\x00\x51\xe3\x0b\x00\x00\x0a'
    b'\x81\x11\x86\xe0\x0e\x30\x86\xe0\x0e\x10\x81\xe0\x04\x20\xd3\xe5'
    b'\x08\x30\x83\xe2\x17\x00\x52\xe3\x08\x00\x13\x05\x00\x20\x96\x07'
    b'\x02\x20\x86\x00\x00\x20\x86\x07\x01\x00\x53\xe1\xf6\xff\xff\x1a'
    b'\x00\x30\xa0\xe3\x7e\xff\x17\xee\xfd\xff\xff\x1a\x9a\x3f\x07\xee'
    b'\x15\x3f\x07\xee\x44\x30\x9d\xe5\x03\x00\x86\xe0\x64\xd0\x8d\xe2'
    b'\xf0\x8f\xbd\xe8\x04\x00\xa0\xe1\x74\xff\xff\xeb\x00\x00\xa0\xe3'
    b'\x64\xd0\x8d\xe2\xf0\x8f\xbd\xe8\x10\x10\x9d\xe5\x00\x20\xa0\xe3'
    b'\x04\x00\xa0\xe1\x73\xff\xff\xeb\x1c\x20\x9d\xe5\x04\x30\xa0\xe1'
    b'\x0b\x10\xa0\xe1\x08\x00\xa0\xe1\x6b\xff\xff\xeb\x1c\x30\x9d\xe5'
    b'\x20\x20\x9d\xe5\x03\x00\x52\xe1\xa9\xff\xff\x9a\x03\x30\x88\xe0'
    b'\x02\x80\x88\xe0\x00\x20\xa0\xe3\x01\x20\xc3\xe4\x08\x00\x53\xe1'
    b'\xfc\xff\xff\x1a\xa2\xff\xff\xea\x04\x00\xa0\xe1\x5b\xff\xff\xeb'
    b'\xda\xff\xff\xea\x68\x02\x00\x00\x7f\x45\x00\x00\x72\x00\x62\x00'
    b'\x00\x00'
)

class ShellcodeElfLoader:
    def __init__(self, dbg):
        self.dbg = dbg
        # how many ARM instructions a run may execute before the launcher gives
        # up.  A frame is ~35k instructions of editor work on this image, so this
        # is ~1100 frames of headroom: enough that a normal session never hits
        # it, small enough that a bug cannot spin the calculator's CPU forever.
        # The debug shim honours it (see calc/main.py's FakeDebug in the tests).
        self.max_insn = 40000000
        self.image_raw = 0
        self.image_base = 0
        self.image_size = 0
        self.entry = 0
        self.ring = 0
        self.owned = False
        self.image_zeroed = False
        self.state_addr = 0
        self.blob_addr = 0

    def forget(self):
        """Drop the image's bookkeeping (NOT the memory: see unload)."""
        self.image_raw = 0
        self.image_base = 0
        self.image_size = 0
        self.entry = 0
        self.ring = 0
        self.owned = False
        self.held = False

    def alloc_image(self, size):
        """The image block, zeroed if the firmware can manage it.

        Calloc first (svc 0x10038): the .bss of the image is then already zero,
        which is 116 KB that would otherwise cross the debug channel a word at a
        time.  `calloc(n, 1)` is used rather than `calloc(1, n)` so that a
        firmware whose calloc takes a single size argument still allocates the
        right number of bytes.
        """
        self.image_zeroed = False
        try:
            p = self.dbg.call(ADDR_CALLOC, size, 1)
        except Exception as e:
            note('[load] calloc is not usable here (%r): malloc + clear' % (e,))
            p = 0
        if p:
            self.image_zeroed = True
            return p
        return self.dbg.call(ADDR_MALLOC, size)

    def name_candidates(self, appdir):
        """The image's name, as the FIRMWARE's file API wants it.

        Its fopen has been measured to want the full device path (primetcc's
        notes), while MicroPython is the API that refuses those -- so the device
        spelling comes first and the bare name is the fallback, exactly as the C
        side opens its launch file.
        """
        out = []
        if appdir:
            out.append(appdir.rstrip('\\') + '\\' + IMAGE_NAME)
        out.append(IMAGE_NAME)
        return out

    def copy_segments_fw(self, appdir, data):
        """Copy the PT_LOAD segments with the firmware's fread.  True on success.

        THE ONLY THING THAT MATTERS IS THE BYTES, so that is what is checked: the
        first, middle and last 8 bytes of every segment are read back and
        compared with the file.  The return value is NOT trusted as a count --
        the firmware counts ITEMS, and which argument is the size and which is
        the count is not documented the same way in every source ("size, nmemb"
        in C's fread, but this machine's own loader reads its ELF header with
        (52, 1) and compares the result with 1).  Both argument orders are tried
        and the read-back decides; both give the same number of bytes, so a
        working firmware is accepted either way and a broken one is caught here
        rather than inside the image.

        ANY failure ends in the caller's word at a time path, and every reason is
        logged: on the calculator the log is the only witness, and "why was the
        load slow" has to be answerable from it.
        """
        if not self.image_base or not data:
            return False
        segs = [t for t in ElfTools.segments(data) if t[2]]
        if not segs:
            return False
        mem = MemUtils(self.dbg)
        for name in self.name_candidates(appdir):
            p = mem.alloc_wstr(name)
            m = mem.alloc_wstr('rb')
            fd = 0
            try:
                if p and m:
                    fd = self.dbg.call(FW_FOPEN, p, m)
            except Exception as e:
                alert('[load] fopen(%s) raised: %r' % (name, e))
                fd = 0
            mem.free(p)
            mem.free(m)
            if not fd:
                note('[load] fopen(%s, rb) refused: trying the next spelling'
                    % name)
                continue
            try:
                ok = self.copy_segments_fd(fd, data, segs)
            except Exception as e:
                alert('[load] the firmware copy raised: %r' % (e,))
                ok = False
            finally:
                try:
                    self.dbg.call(FW_FCLOSE, fd)
                except Exception:
                    pass
            if ok:
                log('[load] segments copied by the firmware from %s' % name)
                return True
        return False

    def copy_segments_fd(self, fd, data, segs):
        """One segment at a time through `fd`, each one verified."""
        for (p_off, p_vaddr, p_filesz, _p_memsz) in segs:
            if not self.read_into(fd, p_off, self.image_base + p_vaddr,
                                  p_filesz, data):
                return False
        return True

    def read_into(self, fd, off, dest, size, data):
        """fread `size` bytes at file offset `off` into `dest`, verified.

        THREE ATTEMPTS, because the firmware's fread has two things about it that
        no source here states the same way: which argument is the size and which
        the item count, and whether it will move a whole segment (471 KB) in one
        call.  All three end in the SAME check -- bytes in memory against bytes in
        the file -- so a firmware that behaves differently is discovered here
        instead of quietly sending the load down the word at a time path.

        (That is what "the load is slow" was: one theory of fread, a return value
        compared against 1, and a silent fall back to 124,000 debug writes.)
        """
        for chunk in (size, 65536, 8192):
            if chunk > size:
                continue
            if self.read_chunked(fd, off, dest, size, chunk, data):
                return True
            if chunk == size:
                break                       # the chunk sizes below are smaller
        return False

    def read_chunked(self, fd, off, dest, size, chunk, data):
        """`size` bytes in `chunk` sized pieces, each one verified."""
        done = 0
        while done < size:
            n = min(chunk, size - done)
            if not self.seek_and_read(fd, off + done, dest + done, n, data,
                                      off + done, n):
                return False
            done += n
        if chunk != size:
            log('[load] read %d B in %d byte chunks' % (size, chunk))
        return True

    def seek_and_read(self, fd, file_off, dest, n, data, want_off, want_len):
        """One fseek + fread, in both argument orders, checked against the file."""
        try:
            self.dbg.call(FW_FSEEK, fd, file_off, 0)
        except Exception as e:
            alert('[load] fseek(%d) raised: %r' % (file_off, e))
            return False
        for (sz, nm) in ((n, 1), (1, n)):
            try:
                got = self.dbg.call(FW_FREAD, dest, sz, nm, fd)
            except Exception as e:
                alert('[load] fread(%d, %d) raised: %r' % (sz, nm, e))
                continue
            if not got:
                alert('[load] fread(%d, %d) returned 0' % (sz, nm))
                continue
            if self.window_matches(data, want_off, dest, want_len):
                return True
            log('[load] fread(%d, %d) returned %d but the bytes are wrong'
                % (sz, nm, got))
        return False

    def window_matches(self, data, off, dest, size):
        """The first, middle and last bytes of a window against the file."""
        for start in (0, max(0, size // 2 - 4), max(0, size - 8)):
            n = min(8, size - start)
            if n <= 0:
                continue
            want = data[off + start:off + start + n]
            try:
                got = bytes(self.dbg.read_mem(dest + start, n))
            except Exception as e:
                alert('[load] read-back at +%d raised: %r' % (start, e))
                return False
            if got[:len(want)] != want:
                alert('[load] read-back at +%d differs from the file: %r vs %r'
                    % (start, got[:8], want[:8]))
                return False
        return True

    def load_via_blob(self, data, appdir):
        """Load the segments with the firmware's own loader.  True on success.

        THE FIRST CHOICE, because it is the only one that costs a handful of round
        trips instead of a hundred thousand: the blob below does the reading and
        the relocations in ARM code, and Python only says where the file is.

        It is VERIFIED like everything else here: the entry it returns has to be in
        the image's memory, and the first bytes of the first segment have to match
        the file.  A blob that did nothing (or the wrong thing) falls through to
        the slower paths rather than producing an image that crashes later.
        """
        if not self.image_base or not data:
            return False
        segs = [t for t in ElfTools.segments(data) if t[2]]
        if not segs:
            return False
        mem = MemUtils(self.dbg)
        blob = self.blob_addr
        if not blob:
            # UPLOADED ONCE PER SESSION: 690 bytes cost ~175 debug round trips,
            # which is a rounding error next to the copy it replaces but is worth
            # keeping when a session loads twice
            blob = mem.malloc(len(LOADER_CODE))
            if not blob:
                alert('[load] no memory for the in-firmware loader (%d B)'
                    % len(LOADER_CODE))
                return False
            self.dbg.write_mem_bytes(blob, LOADER_CODE)
            self.blob_addr = blob
        try:
            for name in self.name_candidates(appdir):
                p = mem.alloc_wstr(name)
                if not p:
                    continue
                try:
                    entry = self.dbg.call(blob, p, self.image_base)
                finally:
                    mem.free(p)
                if not entry or entry < 0x30000000 or entry >= 0x32000000:
                    alert('[load] the in-firmware loader returned 0x%08X for %s'
                        % (entry, name))
                    continue
                # VERIFY: the entry has to be inside the image, and the image's
                # first bytes have to be the file's
                off, vaddr, _filesz, _memsz = segs[0]
                want = data[off:off + 16]
                got = bytes(self.dbg.read_mem(self.image_base + vaddr,
                                              len(want)))
                if got != want:
                    alert('[load] the in-firmware loader did not copy the file '
                        '(%r vs %r)' % (got[:8], want[:8]))
                    continue
                if not (self.image_base <= entry < self.image_base +
                        ElfTools.memory_size(data)):
                    log('[load] the in-firmware loader returned an entry '
                        'outside the image (0x%08X)' % entry)
                    continue
                log('[load] loaded by the in-firmware loader from %s' % name)
                self.entry = entry
                return True
        finally:
            pass                    # the blob stays: the next load reuses it
        return False

    def copy_segments_py(self, data):
        """The word at a time copy: what the fast path falls back to.

        One debug call per FOUR BYTES -- 124,000 of them for this image -- so it
        reports what it cost.  Nothing should reach this path on the calculator;
        if the log shows it, the firmware refused every spelling of fread, and
        that number is the explanation for a slow start.
        """
        chunk = 512                         # one debug call per 128 words
        total = 0
        for (p_off, p_vaddr, p_filesz, _p_memsz) in ElfTools.segments(data):
            dest = self.image_base + p_vaddr
            i = 0
            while i < p_filesz:
                n = min(chunk, p_filesz - i)
                self.dbg.write_mem_bytes(dest + i, data[p_off + i:p_off + i + n])
                i += n
                total += n
        log('[load] segments copied from Python: %d B in %d debug call(s)'
            % (total, (total + 3) // 4))

    def clear_bss(self, data):
        """Zero every segment's .bss tail.  Word at a time: there is no firmware
        memset in the service table, and a fresh malloc block is NOT zero -- the
        heap hands back reused memory.

        This is the expensive half of the slow path (one debug call per four
        bytes), so it says what it cost: "the load is slow" has to be answerable
        from the log, and this is the number that answers it.
        """
        total = 0
        for (_p_off, p_vaddr, p_filesz, p_memsz) in ElfTools.segments(data):
            dest = self.image_base + p_vaddr
            for j in range(p_filesz, p_memsz, 512):
                n = min(512, p_memsz - j)
                self.dbg.write_mem_bytes(dest + j, b'\x00' * n)
                total += n
        if total:
            log('[load] cleared %d B of .bss the slow way: %d debug call(s) '
                '(calloc would have made this free)'
                % (total, (total + 3) // 4))

    def slot_armed(self):
        """Is the firmware's get_event slot still pointing into this image?

        True, False, or None when the slot cannot be read.  It is the second half
        of "may this block be freed": a slot that still holds our trap can enter
        this image at ANY moment, so the block has to stay mapped even when no
        hook call is inside it right now.
        """
        try:
            w0 = struct.unpack('<I', bytes(self.dbg.read_mem(FW_EVENT_SLOT, 4)))[0]
        except Exception:
            return None
        return w0 == SLOT_TRAP

    def hook_busy(self):
        """Hook calls in flight, 0 when there is no state block, None if unreadable."""
        if not self.state_addr:
            return 0            # no state: the image never got as far as the hook
        try:
            return struct.unpack('<i', bytes(
                self.dbg.read_mem(self.state_addr + HOOK_BUSY_OFF, 4)))[0]
        except Exception:
            return None

    def unload(self, keep=True):
        """Give the image's block back -- or keep it for the next run.

        `keep=True` (the default, and what a finished session does) leaves the
        image MAPPED: the next run re-enters it instead of loading it, which is the
        difference between a start-up that costs a few hundred debug round trips
        and one that costs none.  Loading is the expensive phase -- the ELF is
        copied into the firmware heap and its relocations applied -- and none of it
        changes between runs of the same file.  604 KB staying mapped on a 32 MB
        machine is a good trade for that; `main.unload()` and a different image
        (release_held(), before the next load) are what give it back.

        UNLESS THE OS IS STILL INSIDE IT -- and that is why keeping is also the
        safe answer.

        THE ONE THING THAT MUST NOT HAPPEN HERE.  The OS's input thread reaches
        the editor through the patched get_event slot, and the call the hook makes
        (the firmware's own get_event) BLOCKS until an event arrives.  So quitting
        the editor usually leaves that thread sitting inside this image, and it
        returns on the NEXT event -- wherever the image happens to be by then.
        Freeing the block here is a use-after-free with a delay, and on this
        calculator the delay ends in a reboot: that is what "esc to the workspace
        list, esc again, and the machine restarts" was.

        The state block's counter decides.  Zero: free.  Non-zero (or unreadable):
        keep the block in `_held` and try again before the next load, by which
        time the user has pressed something and the hook has returned.  Holding
        649 KB for a few seconds beats resetting the calculator.
        """
        if not (self.owned and self.image_raw):
            self.forget()
            return
        busy = self.hook_busy()
        armed = self.slot_armed()
        if not keep and busy == 0 and armed is False:
            global _held
            self.dbg.call(ADDR_FREE, self.image_raw)
            if self.blob_addr:
                self.dbg.call(ADDR_FREE, self.blob_addr)
                self.blob_addr = 0
            log('[hook] image block 0x%08X given back' % self.image_raw)
            # a block that is freed is not resident: drop its bookkeeping
            if _held:
                _held = [h for h in _held if h[0] != self.image_raw]
            self.forget()
        else:
            # KEPT MAPPED, AND STILL USABLE.  The block is not freed and the
            # loader keeps its bookkeeping, so the next run in this session can
            # RE-ENTER the image instead of loading it again -- which is the
            # difference between a second start-up being instant and being another
            # few hundred (or hundred thousand) debug round trips.  It goes back
            # through release_held() before any future load.
            _held.append((self.image_raw, self.image_size, self.state_addr))
            self.held = True
            log('[hook] keeping the image at 0x%08X mapped (hook calls in '
                'flight: %r, slot still armed: %r): the next run reuses it'
                % (self.image_raw, busy, armed))

    def release_held(self):
        """Free the blocks kept for an in-flight hook call, now that it is over."""
        global _held
        keep = []
        for (raw, size, state) in _held:
            busy = None
            if state:
                try:
                    busy = struct.unpack('<i', bytes(
                        self.dbg.read_mem(state + HOOK_BUSY_OFF, 4)))[0]
                except Exception:
                    busy = None
            # the slot has to be clear too: a block that the OS can still enter
            # is not freeable however idle it looks right now
            if busy == 0 and self.slot_armed() is False:
                self.dbg.call(ADDR_FREE, raw)
                log('[hook] released the held image block 0x%08X' % raw)
                # a block that has been freed is not resident any more
                if _resident.loader is not None and \
                        _resident.loader.image_raw == raw:
                    _resident.entry = 0
                    _resident.loader = None
            else:
                keep.append((raw, size, state))
        _held = keep
        if _held:
            alert('[hook] %d image block(s) still held: a hook call is in flight'
                % len(_held))

    def load(self, data, appdir=None):
        """Load `data` (the whole ELF) and return the entry address, or 0.

        Nothing is reserved past the image: the launch config is a FILE the
        launcher writes next to the image (src/nb_cfg.h), not bytes appended to
        the loaded block.  An earlier version did reserve room in the image's
        .bss tail, and the config read back as zeros on the device -- the loader
        clears that tail and the firmware heap reuses it.
        """
        need = ElfTools.memory_size(data)
        if not need:
            alert('[load] FAIL: not an ELF image')
            return 0
        # a block kept because the OS was inside the hook is given back here: by
        # now the user has pressed something and that call has returned
        self.release_held()
        entry_off = ElfTools.entry_point(data)
        relocs, unknown = ElfTools.relocations(data)
        if unknown:
            # the loader cannot apply a symbol reloc: an image with one would
            # run with a zero where an address belongs, and crash far away
            alert('[load] FAIL: %d symbol relocation(s): rebuild with make '
                '(tools/fix_relocs.py folds them)' % unknown)
            return 0
        log('[load] image needs %d B, %d reloc(s)' % (need, len(relocs)))
        if not (self.owned and self.image_size >= need):
            if self.owned and self.image_raw:
                self.dbg.call(ADDR_FREE, self.image_raw)
            # CALLOC, NOT MALLOC.  The firmware's allocation next to malloc
            # (svc 0x10038; primetcc's service list documents it) hands back a
            # ZEROED block, which is exactly what a freshly loaded image's .bss
            # has to be -- and it saves writing those 116 KB one word at a time
            # through the debug channel.  When the service is not there, the
            # block comes from malloc and clear_bss() does the work.
            self.image_raw = self.alloc_image(need + 16)
            if not self.image_raw:
                alert('[load] FAIL: the firmware gave no memory for %d B'
                    % (need + 16,))
                return 0
            self.image_base = (self.image_raw + 15) & ~7
            self.image_size = need
            self.owned = True
            log('[load] image block 0x%08X base 0x%08X%s' %
                (self.image_raw, self.image_base,
                 ' (calloc: zeroed)' if self.image_zeroed else ' (malloc)'))

        # THE SEGMENTS: the firmware copies them if it can.
        #
        # The obvious way is to write them from Python, and the debug protocol
        # takes ONE WORD per call: ~124,000 round trips for this image, against
        # 61 for the relocations and a handful for everything else.  The
        # firmware's own fread takes a destination pointer, so one call per
        # segment does the same work.  The slow path stays as the fallback,
        # because a fast path that half worked would be worse than a slow one.
        # THREE WAYS TO GET THE IMAGE IN, best first.  The in-firmware loader does
        # the whole job in ARM code (including the relocations), the SVC copy uses
        # the firmware's fread if it will take a destination pointer, and the last
        # one writes four bytes per debug round trip -- 121,875 of them for this
        # image, which is what a start-up measured before the blob was used.
        by_blob = self.load_via_blob(data, appdir)
        if not by_blob:
            if not self.copy_segments_fw(appdir, data):
                self.copy_segments_py(data)
        if self.image_zeroed:
            log('[load] .bss comes zeroed from the firmware: nothing to clear')
        elif not by_blob:
            self.clear_bss(data)

        if by_blob:
            # THE BLOB APPLIED THEM.  Applying them again would add the base twice
            # to every relocated word, which is an image that starts and then
            # jumps into nowhere -- so this is skipped, not repeated.
            log('[load] relocations were applied by the in-firmware loader '
                '(%d of them)' % len(relocs))
        else:
            for (r_off, _rtype, _sym) in relocs:
                addr = self.image_base + r_off
                word = self.dbg.read_mem(addr, 4)
                if len(word) < 4:
                    alert('[load] FAIL: cannot read the reloc word at 0x%08X'
                        % addr)
                    self.unload()
                    return 0
                value = struct.unpack('<I', bytes(word))[0]
                self.dbg.write_mem(addr,
                                   (value + self.image_base) & 0xFFFFFFFF)

        self.entry = self.image_base + entry_off
        # where the input state is, so unload() can ask whether the OS is inside
        # the hook without needing the file bytes again
        self.state_addr = ElfTools.find_magic(data, b'NBINPUT', self.image_base)
        # `ring` is resolved by the caller from the config area's published
        # address (see run_file); find_magic is only a fallback, because the
        # first occurrence of those bytes in the file is not necessarily the
        # ring -- the config area's string data can contain it
        self.ring = ElfTools.find_magic(data, b'NBCORING', self.image_base)
        self.seg_top = ElfTools.seg_end(data)
        log('[load] entry 0x%08X ring 0x%08X image_end 0x%08X'
            % (self.entry, self.ring, self.image_base + self.seg_top))
        if not self.ring:
            alert('[load] FAIL: no NBCORING ring in the image')
            self.unload()
            return 0
        return self.entry


# ---------------------------------------------------------------------------
# the launch file the editor reads (src/nb_cfg.h -- the two must agree)
# ---------------------------------------------------------------------------
CFG_MAGIC = 0x6763626E          # "nbcg"
CFG_VERSION = 4
CFG_MAX_WS = 8
CFG_MAX_FILES = 512          # entries, across every directory
CFG_MAX_DIRS = 64            # a workspace plus the folders inside it: the
                             # budget has to clear C:\DATA's .hpappdir folders
                             # (24 was not enough, and the rows that missed the
                             # cut fell back to the application folder)
CFG_NO_DIR = 0xFFFFFFFF
CFG_MAX_NAMES = 512          # names and paths, in the pool the C side shares
CFG_NAME_MAX = 64            # one of them
CFG_DIR_DEPTH = 2            # how deep to follow folders (a workspace, its
                             # folders, and theirs: C:\DATA and the app folders
                             # in it are depth 1, which is what a user browses)

# THE LAUNCH FILE (src/nb_cfg.h): a header of little-endian words and then
# NUL-terminated strings.  The launcher writes it; the editor reads it with the
# firmware's own file API.  THERE IS NO SHARED STRUCT LAYOUT -- that is the point
# of the format, and the reason the launcher no longer cares how wide a pointer
# is on the ARM target or where a field sits inside anything.
CFG_FILE = 'nbcode.cfg'

# the image and the durable log, by bare name: both are opened from Python with
# a bare name (the firmware resolves it against the application folder) and from
# the C side with a device path, which is what log_device_path() builds
IMAGE_NAME = 'nbCode.elf'
LOG_TAIL = 'nbcode.log'
# the editor's global clipboard (src/nb_clip.c): ten entries and their file, which
# lives next to the log.  The file is nbCode's own, like the log and the launch
# file, so the browser does not offer it -- a user who edits the clipboard by hand
# is a user who has lost it.
CLIP_TAIL = 'nbclip.txt'


def log_device_path(appdir):
    """The durable log as the C SIDE should open it.

    The image writes that file with the FIRMWARE's file API, which takes a full
    device path -- so the launcher hands it one.  When the launcher could not
    learn the application folder (no W_GETCWD, no os.getcwd), the bare name goes
    in instead: the C side tries that spelling first, exactly as the launcher
    does.
    """
    if not appdir:
        return LOG_TAIL
    return appdir.rstrip('\\') + '\\' + LOG_TAIL


def build_config(settings, workspaces, dirs, cwd, open_file='', logpath='',
                 frames=0):
    """the launch file's bytes, ready to write next to the image.

    `dirs` is a list of directories, each `{'path':..., 'parent':index or None,
    'files':[{'name','bytes','isdir','child'}...]}`: the workspaces first, then the
    folders inside them, which is what lets the browser show a folder row and step
    into it (src/nb_cfg.h).  A file's name is relative to its own directory; a
    folder's row carries the index of the directory it points at.
    """
    flags = 0
    if settings.get('readonly'):
        flags |= 0x0001
    if not settings.get('highlight', True):
        flags |= 0x0002
    if not settings.get('show_line_numbers', True):
        flags |= 0x0004
    if not settings.get('complete', True):
        flags |= 0x0008		# completion off (it is on by default)
    if not settings.get('indent_guides', True):
        flags |= 0x0010		# indentation guides off (on by default)
    lang = settings.get('language', 'auto')
    if isinstance(lang, str):
        lang = LANG_NAMES.get(lang.lower(), 0)
    tab = settings.get('tab_width', 4)
    if not isinstance(tab, int) or tab < 2 or tab > 8:
        tab = 4
    ws = [w[1] for w in workspaces][:CFG_MAX_WS]

    out = bytearray()
    out += struct.pack('<IIIIIIIII', CFG_MAGIC, CFG_VERSION, flags,
                      1 if settings.get('theme') else 0, tab, int(lang),
                      int(frames or 0), len(ws),
                      int(settings.get('default_workspace', 0) or 0))
    out += utf8_bytes(cwd) + b'\x00'
    out += utf8_bytes(open_file or '') + b'\x00'
    out += utf8_bytes(logpath or '') + b'\x00'
    for w in ws:
        out += utf8_bytes(w) + b'\x00'
    out += struct.pack('<I', len(dirs))
    for d in dirs:
        out += utf8_bytes(str(d.get('path', ''))) + b'\x00'
        parent = d.get('parent')
        out += struct.pack('<I', 0xFFFFFFFF if parent is None else int(parent))
        files = d.get('files') or []
        out += struct.pack('<I', len(files))
        for f in files:
            out += struct.pack('<II', int(f.get('bytes', 0)),
                               1 if f.get('isdir') else 0)
            out += utf8_bytes(str(f.get('name', ''))) + b'\x00'
    return bytes(out)


def write_config(path, data):
    """write the launch file (the editor reads it with the firmware's API)."""
    with io.FileIO(path, 'wb') as f:
        f.write(data)
    return len(data)



# settings: nbCode.hpappdir\setting.json
# ---------------------------------------------------------------------------
DEFAULTS = {
    "theme": 0,
    "tab_width": 4,
    "show_line_numbers": True,
    "highlight": True,
    "readonly": False,
    "language": "auto",
    "workspaces": [],
    "default_workspace": 0,
    "shellcode": 0xE51FF004,
}

LANG_NAMES = {"auto": 0, "ppl": 1, "lua": 2, "py": 3, "python": 3, "c": 4,
              "txt": 5, "text": 5}


# ---------------------------------------------------------------------------
# setting.json WITHOUT A JSON MODULE
#
# THE CALCULATOR HAS NO json AND NO ujson.  (primeLua's launcher never parses
# JSON at all, which is how its settings work: a slot file and command
# arguments.)  nbCode's settings are user-edited JSON, so the launcher carries a
# parser for the shape that file is written in: one object, string keys, and
# scalars / strings / arrays as values.  It is a real parser (not a regex) so
# that a hand-edited file with odd spacing, escapes or nesting still reads, and
# it is used whenever the platform HAS no json module.
#
# A file it cannot parse is not fatal: read_settings logs it and uses the
# defaults, because a launcher that refuses to start over a stray comma is a
# brick.
# ---------------------------------------------------------------------------
class JsonError(Exception):
    pass


_JSON_WS = ' \t\r\n'
_JSON_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', 'b': '\b', 'f': '\f',
                 '/': '/', '\\': '\\', '"': '"'}


def _json_skip(s, i):
    while i < len(s) and s[i] in _JSON_WS:
        i += 1
    return i


def _json_string(s, i):
    i += 1                                   # past the opening quote
    out = []
    while True:
        if i >= len(s):
            raise JsonError('unterminated string')
        c = s[i]
        if c == '"':
            return ''.join(out), i + 1
        if c == '\\':
            i += 1
            if i >= len(s):
                raise JsonError('unterminated escape')
            e = s[i]
            if e == 'u':
                hex4 = s[i + 1:i + 5]
                if len(hex4) < 4:
                    raise JsonError('truncated \\u escape')
                out.append(chr(int(hex4, 16)))
                i += 5
                continue
            out.append(_JSON_ESCAPES.get(e, e))
            i += 1
            continue
        out.append(c)
        i += 1


def _json_value(s, i):
    i = _json_skip(s, i)
    if i >= len(s):
        raise JsonError('no value')
    c = s[i]
    if c == '{':
        obj = {}
        i = _json_skip(s, i + 1)
        if i < len(s) and s[i] == '}':
            return obj, i + 1
        while True:
            i = _json_skip(s, i)
            if i >= len(s) or s[i] != '"':
                raise JsonError('object key expected at %d' % i)
            key, i = _json_string(s, i)
            i = _json_skip(s, i)
            if i >= len(s) or s[i] != ':':
                raise JsonError('":" expected at %d' % i)
            val, i = _json_value(s, i + 1)
            obj[key] = val
            i = _json_skip(s, i)
            if i < len(s) and s[i] == ',':
                i += 1
                continue
            if i < len(s) and s[i] == '}':
                return obj, i + 1
            raise JsonError('"," or "}" expected at %d' % i)
    if c == '[':
        arr = []
        i = _json_skip(s, i + 1)
        if i < len(s) and s[i] == ']':
            return arr, i + 1
        while True:
            val, i = _json_value(s, i)
            arr.append(val)
            i = _json_skip(s, i)
            if i < len(s) and s[i] == ',':
                i += 1
                continue
            if i < len(s) and s[i] == ']':
                return arr, i + 1
            raise JsonError('"," or "]" expected at %d' % i)
    if c == '"':
        return _json_string(s, i)
    for lit, val in (('true', True), ('false', False), ('null', None)):
        if s[i:i + len(lit)] == lit:
            return val, i + len(lit)
    j = i
    while j < len(s) and s[j] in '-+0123456789.eE':
        j += 1
    if j == i:
        raise JsonError('unexpected %r at %d' % (c, i))
    text = s[i:j]
    try:
        return int(text), j
    except ValueError:
        try:
            return float(text), j
        except ValueError:
            raise JsonError('bad number %r' % text)


def parse_json(text):
    """The value in `text`, or raise JsonError."""
    value, i = _json_value(text, 0)
    i = _json_skip(text, i)
    if i != len(text):
        raise JsonError('trailing text at %d' % i)
    return value


def setting_path(appdir):
    return appdir + '\\setting.json'


# WHERE nbCode IS INSTALLED, as the DEVICE names it.
#
# The launcher has to know this for two different reasons, and neither can be
# answered by MicroPython: the editor wants paths it can open with the firmware's
# file API, and the workspace list should offer the folder the editor lives in.
# A bare name is not enough for either -- the C side needs a path, and the list
# needs something to display -- so it is asked of the firmware (W_GETCWD), with
# the host's own cwd and then the build-time constant as fallbacks.
APP_DIRS = ['C:\\DATA\\nbCode.hpappdir', 'C:\\DATA', '\\']


def find_app_dir(dbg=None):
    """The application folder as a DEVICE path, or APP_DIR as a last resort.

    W_GETCWD alone is not trusted: the answer only counts when nbCode.elf is
    really inside it.  (primetcc found that the Python application's cwd can be
    the PARENT of the app folder; a naive answer then produces a nested
    "<app>\\<app>\\" path, so that shape is checked too.)
    """
    if dbg is not None:
        d = fw_cwd(dbg)
        if d:
            for cand in (d.rstrip('\\'),
                         d.rstrip('\\') + '\\' + APP_DIR_NAME):
                if fw_exists(dbg, cand + '\\' + IMAGE_NAME):
                    log('[probe] app folder (firmware): %s' % cand)
                    return cand
            log('[probe] the firmware cwd is %s, but %s is not in it'
                % (d, IMAGE_NAME))
    try:
        import os
        d = os.getcwd()
        if d and os.path.isfile(os.path.join(d, IMAGE_NAME)):
            return d
    except Exception:
        pass
    return APP_DIR


def fw_exists(dbg, path):
    """True when the FIRMWARE can see `path` (W_GET_ATTR answers -1 for none).

    The call primetcc and primeLua use to stat a file, and the only way to ask
    about a path outside the application folder.
    """
    try:
        mem = MemUtils(dbg)
        p = mem.alloc_wstr(path)
        if not p:
            return False
        try:
            return dbg.call(FW_GET_ATTR, p) != 0xFFFFFFFF
        finally:
            mem.free(p)
    except Exception:
        return False


def fw_cwd(dbg):
    """The firmware's current directory, as a device path, or "".

    GET_DISK_ID tells it which disk, W_GETCWD fills a wide-string buffer with the
    path.  On this machine the Python application's directory IS the application
    folder, which is why this is the launcher's answer to "where am I installed".
    """
    try:
        mem = MemUtils(dbg)
        disk = dbg.call(FW_GET_DISK_ID)
        buf = mem.malloc(520)
        if not buf:
            return ''
        try:
            if dbg.call(FW_GETCWD, disk, buf) == 0xFFFFFFFF:
                return ''
            return mem.read_wstr(buf).rstrip('\\')
        finally:
            mem.free(buf)
    except Exception:
        return ''


def read_bytes(name, appdir=None):
    """The bytes of `name`, or None.

    BARE NAME FIRST: that is the spelling this firmware accepts, and the one that
    makes "the file next to main.py" reachable at all.  The device paths are kept
    as fallbacks because a host has no application folder to resolve against --
    they are tried only after the bare name has failed.
    """
    cands = [name]
    if appdir:
        cands.append(appdir.rstrip('\\') + '\\' + name)
    for d in APP_DIRS:
        cands.append(d.rstrip('\\') + '\\' + name)
    for c in cands:
        try:
            with open_file(c, 'rb') as f:
                return f.read()
        except Exception:
            continue
    return None


def read_settings(appdir):
    """The settings file, with every missing key defaulted.

    A JSON syntax error must not stop nbCode from starting: a launcher that
    refuses to run because of a stray comma is a brick.  The error is reported in
    the log and the defaults are used.
    """
    out = dict(DEFAULTS)
    raw = None
    where = ''
    for candidate in ('setting.json', 'setting.json.txt',
                      setting_path(appdir), setting_path(appdir) + '.txt'):
        try:
            with open_file(candidate, 'rb') as f:
                raw = f.read()
            where = candidate
            break
        except Exception:
            raw = None
    if raw is None:
        log('[settings] setting.json not found: using the defaults')
        return out, 'missing'
    if isinstance(raw, bytes):
        if raw[0:3] == b'\xef\xbb\xbf':
            raw = raw[3:]
        try:
            raw = raw.decode('utf-8')
        except Exception:
            raw = raw.decode('latin-1')
    # json where the platform has it (a host), the parser above where it does
    # not (the calculator): the file is user-edited and has to read the same
    # either way
    try:
        if json is not None:
            parsed = json.loads(raw)
        else:
            parsed = parse_json(raw)
    except Exception as e:
        note('[settings] %s is not valid JSON (%r): using the defaults'
            % (where, e))
        return out, 'invalid'
    if not isinstance(parsed, dict):
        note('[settings] %s is not an object: using the defaults' % where)
        return out, 'invalid'
    for k in parsed:
        out[k] = parsed[k]
    log('[settings] %s: %d key(s)' % (where, len(parsed)))
    return out, 'ok'


def write_settings(appdir, settings):
    """Write setting.json back (the editor never does: settings are the
    launcher's business, and a file the user edits by hand stays theirs).

    Needs a json module to serialise: the calculator has none, and nothing on
    the device calls this, so it says so instead of writing half a file.
    """
    if json is None:
        alert('[settings] no json module: not writing setting.json')
        return False
    try:
        text = json.dumps(settings)
    except Exception:
        return False
    for name in ('setting.json', setting_path(appdir)):
        try:
            with open_file(name, 'wb') as f:
                f.write(utf8_bytes(text))
            return True
        except Exception:
            continue
    return False


# ---------------------------------------------------------------------------
# files
# ---------------------------------------------------------------------------
def read_text(path):
    raw = read_bytes(path)
    if raw is None:
        return None
    if isinstance(raw, bytes):
        for enc in ('utf-8', 'gb2312', 'latin-1'):
            try:
                return raw.decode(enc)
            except Exception:
                continue
        return decode_bytes(raw)
    return raw


# The launcher's OWN directory, which is also the process's working directory on
# the calculator: a bare name in list_dir()/stat() means a file next to main.py,
# and that is the only way MicroPython's file API can be asked for one.
APP_DIR_NAME = 'nbCode.hpappdir'

# the finder's answer block: 40 bytes, and these are the only fields that matter
# to a file list.  The offsets are the firmware's own layout (primeLua read them
# off the device and uses the same ones).
FIND_CTX_BYTES = 256
FIND_LFN_OFF = 8                # pointer to the long (Unicode) name
FIND_SFN_OFF = 12               # pointer to the 8.3 name
FIND_SIZE_OFF = 20
FIND_ATTR_OFF = 37
FIND_ATTR_DIR = 0x10


def parse_find_entry(raw):
    """One finder answer -> {'lfn': ptr, 'sfn': ptr, 'size': n, 'attr': a}.

    Split out as a plain function because it is the part of the device listing
    that CAN be tested on a host: the rest needs the firmware to answer.
    """
    if len(raw) < FIND_ATTR_OFF + 1:
        return None
    lfn = struct.unpack_from('<I', raw, FIND_LFN_OFF)[0]
    sfn = struct.unpack_from('<I', raw, FIND_SFN_OFF)[0]
    size = struct.unpack_from('<I', raw, FIND_SIZE_OFF)[0]
    attr = struct.unpack_from('<B', raw, FIND_ATTR_OFF)[0]
    return {'lfn': lfn, 'sfn': sfn, 'size': size, 'attr': attr}


def merge_find_rows(rows):
    """One row per NAME, from the finder's several passes.

    THE FINDER IS ASKED SEVERAL TIMES ON PURPOSE -- the firmware returns long
    names, hidden files and the rest under different attribute masks -- and every
    pass returns everything it can see, so each file comes back once per mask.
    Without this, a folder with three files listed TWELVE rows: the start screen
    showed every name four times, which is exactly what it did.  (primeLua's
    finder dedupes for the same reason; nbCode's first version did not.)

    The first spelling of a name wins; a pass that reports a size where an
    earlier one had none fills it in.
    """
    out = []
    where = {}
    for row in rows:
        name = row[0]
        key = name.lower()
        if key not in where:
            where[key] = len(out)
            out.append(row)
            continue
        i = where[key]
        if not out[i][1] and row[1]:
            out[i] = (out[i][0], row[1], out[i][2] or row[2])
    return out


def list_dir_fw(dbg, path):
    """[(name, size, isdir)] for `path` using the FIRMWARE's finder.

    This is the only way to list a folder other than the application folder on
    the calculator: MicroPython's file API cannot see outside it, and a file list
    for "C:\\DATA" is the whole point of the workspace screen.  The pattern's
    attribute mask is retried a few times because the firmware needs different
    masks for long names, hidden files and everything-else (primeLua measured
    this) -- and the passes are merged, because each one returns the same files
    again (see merge_find_rows).
    """
    out = []
    mem = MemUtils(dbg)
    pattern = path.rstrip('\\') + '\\*'
    # EVERY MASK, EVERY TIME, AND THE PASSES ARE MERGED -- primeLua's loop,
    # argument for argument (its calc/main.py lists a folder the same way).
    #
    # This used to STOP at the first mask that returned anything, to save the
    # round trips: three passes of a folder that the first pass had already
    # listed are three times the calls for the same names.  That was wrong, and
    # the report was exact -- "the file list only shows 13 entries" -- because the
    # masks are not four views of ONE list: 0x37 sees the ordinary files, and the
    # long names, the hidden ones and the rest come back under the others.  A
    # folder whose tenth entry has a long name showed nine rows and a folder of
    # twenty showed thirteen, with the count in the header agreeing with what was
    # listed because BOTH were short (merge_find_rows is what made one pass look
    # right: it dedupes, so four passes cost calls but no duplicate rows).
    passes = 0
    for mask in (0x37, 0xFF, 0x00, None):
        passes += 1
        p = mem.alloc_wstr(pattern)
        ctx = mem.malloc(FIND_CTX_BYTES)
        if not p or not ctx:
            mem.free(p)
            mem.free(ctx)
            break
        try:
            if mask is None:
                ok = dbg.call(FW_FIND_FIRST, p, ctx)
            else:
                ok = dbg.call(FW_FIND_FIRST, p, ctx, mask)
            if ok != 0:
                continue
            while True:
                ent = parse_find_entry(bytes(dbg.read_mem(ctx, 40)))
                if ent is not None:
                    name = mem.read_wstr(ent['lfn']) or \
                        mem.read_utf8(ent['sfn'])
                    if name and name not in ('.', '..'):
                        out.append((name, ent['size'],
                                    bool(ent['attr'] & FIND_ATTR_DIR)))
                if dbg.call(FW_FIND_NEXT, ctx) != 0:
                    break
        except Exception as e:
            alert('[list] the firmware finder failed on %s: %r' % (path, e))
        finally:
            try:
                dbg.call(FW_FIND_CLOSE, ctx)
            except Exception:
                pass
            mem.free(p)
            mem.free(ctx)
    merged = merge_find_rows(out)
    log('[list] %s: %d entr(ies) from %d finder result(s) over %d mask(s)'
        % (path, len(merged), len(out), passes))
    return merged


def list_dir_os(path):
    """The same list on a host, where the file tree is a real directory."""
    out = []
    import os
    where = path
    tail = path.replace('/', '\\').rstrip('\\')
    if tail.lower().endswith(APP_DIR_NAME.lower()):
        # the application folder IS the process's directory here
        where = '.'
    elif not os.path.isdir(where):
        # A DEVICE PATH UNDER THE APPLICATION FOLDER, by its tail: the launcher is
        # run from INSIDE the application folder on a host (the tests cd() there),
        # so everything below it is reachable by the part after the folder's name.
        # Without this the workspace walk reported every SUBFOLDER as unreadable --
        # it builds "C:\\DATA\\nbCode.hpappdir\\sub" -- which left the whole
        # nested-folder path untested off the calculator.
        i = tail.lower().find('\\' + APP_DIR_NAME.lower() + '\\')
        if i >= 0:
            # the tail is a DEVICE path: its separators are backslashes, and a
            # host needs its own (a nested folder is where this first went wrong)
            cand = tail[i + len(APP_DIR_NAME) + 2:].replace('\\', os.sep)
            if cand and os.path.isdir(cand):
                where = cand
    if not os.path.isdir(where):
        alert('[list] cannot read %s: not a directory' % (path,))
        return out
    try:
        names = os.listdir(where)
    except Exception as e:
        alert('[list] cannot read %s: %r' % (path, e))
        return out
    for fname in names:
        if fname in ('.', '..'):
            continue
        size, isdir = 0, False
        try:
            st = os.stat(os.path.join(where, fname))
            size = st[6] if len(st) > 6 else 0
            isdir = bool(st[0] & 0x4000)
        except Exception:
            pass
        out.append((fname, size, isdir))
    return out


def list_dir(path, dbg=None):
    """[(name, size, isdir)] for `path`, [], when it cannot be read.

    Two backends, because the two environments have different facilities: the
    calculator's firmware finder (any folder on the disk) and the host's os
    module (a real directory tree).  Whichever is used is logged, so a device log
    says which one answered.
    """
    out = []
    if dbg is not None and is_device():
        out = list_dir_fw(dbg, path)
        if out:
            log('[list] %s: %d entr(ies) from the firmware finder'
                % (path, len(out)))
        else:
            log('[list] %s: the firmware finder returned nothing' % path)
    else:
        out = list_dir_os(path)
        log('[list] %s: %d entr(ies) from os.listdir' % (path, len(out)))
    out.sort(key=lambda e: (not e[2], e[0].lower()))
    return out



# ---------------------------------------------------------------------------
# the run itself
# ---------------------------------------------------------------------------
# the image asks for a durable log line with "LOG:" in the ring: the launcher
# appends it to nbcode.log, because the launcher is the side that is still
# running when the image is not
LOG_PREFIX = 'LOG:'
_log_file = None

# WHETHER THE LAUNCHER TALKS TO THE CONSOLE.
#
# The durable log FILE is always written -- it survives a reboot, the image
# appends its milestones to it, and it is what a crash gets diagnosed from -- but
# PRINTING every [load]/[cost]/[hook] line into the calculator's console at
# start-up and again on the way out is noise in the user's face on every single
# run.  An empty file named `verbose` next to main.py turns the printing back on
# (the same idiom as primeLua's `nothread` marker); `probe()` overrides it,
# because that entry point exists to be read.
VERBOSE_MARKER = 'verbose'
_verbose = None


def verbose():
    """Is the launcher printing to the console?  Cached: this is asked for every
    log line, and the answer cannot change while a session runs."""
    global _verbose

    if _verbose is None:
        try:
            with open_file(VERBOSE_MARKER, 'rb') as f:
                f.read(1)
            _verbose = True
        except Exception:
            _verbose = False
    return _verbose


def set_verbose(on):
    global _verbose

    _verbose = True if on else False


def append_log_line(line):
    global _log_file
    try:
        if _log_file is None:
            _log_file = io.FileIO(LOG_TAIL, 'ab')
        _log_file.write(utf8_bytes(line + '\n'))
        return
    except Exception:
        _log_file = None             # a runaway editor must not lock the calculator
    log(line)                        # the same line, the slow but certain way

_session = None


class Session:
    """the loaded image, its heap block and the ring reader."""

    def __init__(self, appdir, ui):
        self.dbg = PrimeDebug()
        self.loader = ShellcodeElfLoader(self.dbg)
        self.appdir = appdir
        self.ui = ui
        self.heap = 0

    def close(self):
        if self.heap:
            self.dbg.call(ADDR_FREE, self.heap)
            self.heap = 0
        self.loader.unload(keep=False)


# ---------------------------------------------------------------------------
# START-UP PROGRESS
#
# A load is a second or two of a blank screen (the image cannot draw until it is
# running), and the launcher is the only side that knows WHAT is happening.  So it
# says so: a handful of short lines, printed whether or not the console is in its
# quiet default, because they are not a log -- they are the thing the user is
# waiting for.  The detailed `[load]`/`[cost]` stream stays behind the `verbose`
# marker (see log()).
#
# The format is deliberately flat (`nbCode 3/6: loading the image`) so it reads the
# same in the calculator's console, in the durable log and in a test.
_PROGRESS = None


def progress(step, total, what):
    """One start-up line, on the console and in the log."""
    global _PROGRESS

    line = 'nbCode %d/%d: %s' % (step, total, what)
    if _PROGRESS != line:
        _PROGRESS = line
    try:
        p_log(line)
    except Exception:
        pass
    log('[progress] %s' % what, force=True)


def alert(text):
    """A line the user has to SEE even in an otherwise quiet session: something
    failed, a fallback was taken, or a file they edited was not understood.

    It is p_error: every caller of this is a failure path.  A fallback that leaves
    the session running (malloc instead of calloc, the default settings because
    the JSON was unreadable) is note(), which is p_warning.

    Routine progress ([load] read N bytes, [hook] armed, [cost] ...) goes through
    log() and stays in the file: the editor owns the screen it draws, and a normal
    start-up has nothing to say to the console.
    """
    log(text, force=True, level=p_error)


def note(text):
    """Something worth a look on a session that carries on anyway: a retry, a
    fallback, a setting that had to be ignored.  This is moreprint's p_warning."""
    log(text, force=True, level=p_warning)


def succeed(text, force=True):
    """A step that WORKED (moreprint's p_pass).  Used sparingly: the line that
    marks an editor session which came back cleanly.  `force=False` keeps it in the
    durable log without putting it on a quiet console (see log())."""
    log(text, force=force, level=p_pass)


def log(text, force=False, level=None):
    """The launcher's own log: a FILE, because a reboot clears the console.

    Two ways to append on purpose.  Append mode is what this wants, but the
    calculator's `uio` is a subset of CPython's and the other launchers here only
    ever write with "wb"/"wb+" -- so when "ab" is refused the line is appended by
    reading the file and writing it back, which is what the C side does with its
    own log (port/nb_fw.c: nb_logfile).  A log that cannot be written is not
    fatal: the session continues.

    THE CONSOLE IS QUIET BY DEFAULT (see verbose()): the file gets every line, and
    only a `verbose` marker or `force` puts one on the screen.  Nothing about a
    normal session needs to be watched -- the editor owns the screen it draws --
    and the lines are still there afterwards in nbcode.log.

    WHAT IS PRINTED goes through `level`, one of the moreprint helpers -- p_log by
    default, p_error for alert(), p_warning for note(), p_pass for succeed() -- so
    how the console LOOKS is the user's business and not something each call site
    decides.
    """
    line = utf8_bytes(text + '\n')
    try:
        with io.FileIO(LOG_TAIL, 'ab') as f:
            f.write(line)
    except Exception:
        try:
            old = read_bytes(LOG_TAIL) or b''
            if len(old) > 16384:                 # keep the tail, not the lot
                old = old[-8192:]
            with io.FileIO(LOG_TAIL, 'wb') as f:
                f.write(old + line)
        except Exception:
            pass
    if not force and not verbose():
        return
    try:
        (level or p_log)(text)
    except Exception:
        pass


def free_heap():
    """how much firmware heap is left, for the editor's status bar."""
    try:
        total = hpprime.eval('mem')
        return int(total) * 1024 if total else 0
    except Exception:
        return 0


# struct nb_ring (port/nb_fw.c): magic[8], count, data[8192].  The offsets here
# are the contract; a mismatch would show the user whatever bytes happened to be
# next to the count, which is why they are named once.
RING_MAGIC = b'NBCORING'
RING_COUNT_OFF = 8
RING_DATA_OFF = 12
RING_SIZE = 8192


def decode_ring(raw):
    """The log ring's bytes -> its lines, and this never raises either.

    THE RING IS A BYTE STREAM READ IN WINDOWS.  `parse_rt_ret` reads the last 64
    bytes, so a window can easily start in the MIDDLE of a character -- and
    nbCode's own messages are Chinese, which is three bytes each.  Decoding line
    by line keeps that damage on the one line that was cut instead of turning the
    whole window into mojibake, and it is why this is not one big decode."""
    return [decode_bytes(line) for line in raw.split(b'\n')]


def parse_rt_ret(dbg, ring, state):
    """the last "RT_RET:<n>" the image wrote, or None.

    The debug interface's return value is not trustworthy on this hardware
    (primetcc measured it reporting 2 whatever the callee returns), so the image
    publishes its exit status through the log ring and the launcher parses that.
    """
    if not ring or state is None:
        return None
    try:
        start = state - 64 if state >= 64 else 0
        n = state - start
        raw = bytes(dbg.read_mem(ring + RING_DATA_OFF + (start % RING_SIZE), n))
    except Exception:
        return None
    code = None
    for line in decode_ring(raw):
        if line.startswith('RT_RET:'):
            try:
                code = int(line[7:].strip())
            except Exception:
                code = None
    return code


def drain_ring(dbg, ring, state):
    """Print whatever the image has logged since `state`.

    THE RING IS A MAILBOX, not a queue: the image only appends, the launcher only
    reads forward, and the count at the head says how much is new -- so the
    traffic is one 4-byte read plus the bytes that actually appeared.

    WHEN it is read matters: the run goes through ONE debug call that does not
    return until the editor exits (see run_file), so this is called after the
    session, not during it.  That is deliberate -- the alternative is primeLua's
    thread mode, where the launcher spins polling the ring for as long as the
    editor is open (it needs that because its Lua scripts print as they run;
    nbCode's editor owns the screen, and the log lines that matter are the ones it
    writes to nbcode.log itself).
    """
    if not ring:
        return state
    try:
        head = struct.unpack('<I', bytes(dbg.read_mem(ring + RING_COUNT_OFF, 4)))[0]
    except Exception:
        return state
    if head == state:
        return state
    if head < state or head - state > RING_SIZE:
        state = head - RING_SIZE if head >= RING_SIZE else 0
    n = head - state
    try:
        raw = bytes(dbg.read_mem(ring + RING_DATA_OFF + (state % RING_SIZE), n))
    except Exception:
        return head
    try:
        text = decode_ring(raw)
    except Exception:
        text = []
    for line in text:
        if line.startswith('RT_RET:'):
            continue
        line = line.rstrip()
        if not line:
            continue
        if line.startswith(LOG_PREFIX):
            # the image wants this line in the durable log (it survives a
            # reset; the ring and the console do not)
            append_log_line('[nbCode] ' + line[len(LOG_PREFIX):])
            continue
        log('[nbCode] ' + line)
    return head


class Resident:
    """The loaded image, kept between runs.

    LOADING IS THE EXPENSIVE PART of a session: the ELF is copied into the
    firmware's heap and its relocations applied, which costs a few hundred debug
    round trips and (if the firmware refuses to do the copying) a hundred thousand.
    None of that changes between runs of the same file, so the block, the entry
    point and the ring are remembered here and the second run re-enters the image
    instead of reloading it -- primeLua's launcher keeps its interpreter resident
    for the same reason.  pCode re-reads its launch file on every entry (see
    pc_run_begin), so a reused image still picks up changed settings.
    """

    def __init__(self):
        self.loader = None
        self.entry = 0
        self.ring = 0
        self.size = 0
        self.name = ''
        self.hits = 0
        self.loads = 0


_resident = Resident()


class RoundTrips:
    """Counts the debug round trips between two marks.

    THERE IS NO CLOCK ON THIS CALCULATOR (the firmware's get_time reboots a G1),
    so the cost that matters -- one write plus one read over the debug channel --
    is counted instead: it is the thing that makes a start-up slow, and printing it
    per phase turns "nbCode is slow" into "the listing is slow" or "the copy is the
    slow part".
    """

    def __init__(self, dbg):
        self.dbg = dbg
        self.marks = []

    def mark(self, what):
        n = getattr(self.dbg, 'round_trips', None)
        if n is None:
            return
        self.marks.append((what, n))

    def report(self):
        if len(self.marks) < 2:
            return
        base = self.marks[0][1]
        prev = base
        for (what, n) in self.marks[1:]:
            log('[cost] %-22s %6d round trip(s)' % (what, n - prev))
            prev = n
        log('[cost] %-22s %6d round trip(s) in total'
            % ('start-up', prev - base))


def run_file(path, ui=None, frames=0, max_insn=None, dbg=None):
    """Write the launch file, load nbCode.elf and run the editor.

    Returns the editor's exit status, or None when the image could not be
    loaded at all.  `ui` carries what the launcher already knows: the app
    folder, the parsed settings and the workspace list.
    """
    dbg = dbg or PrimeDebug()
    cost = RoundTrips(dbg)
    # the launch file and the file listing are BARE NAMES (MicroPython resolves
    # them against the application folder, and that is the only spelling this
    # firmware accepts); the paths that are handed to the C side are device
    # paths, because the FIRMWARE's file API is what opens those
    progress(1, 6, 'finding the application folder')
    appdir = (ui or {}).get('appdir') or find_app_dir(dbg)
    cost.mark('start')
    progress(2, 6, 'reading setting.json')
    settings = (ui or {}).get('settings') or read_settings(appdir)[0]
    workspaces = (ui or {}).get('workspaces') or default_workspaces(appdir)
    cwd = (ui or {}).get('cwd') or workspaces[
        min(settings.get('default_workspace', 0) or 0,
            len(workspaces) - 1)][1]

    progress(3, 6, 'listing the workspaces and their folders')
    image = read_bytes(IMAGE_NAME, appdir)
    if image is None:
        alert('[load] FAIL: cannot read %s (tried a bare name and the '
            'application folders)' % IMAGE_NAME)
        return None
    log('[load] %s %d B' % (IMAGE_NAME, len(image)))

    # THE FILE LISTING, PER WORKSPACE.  The firmware's finder is the only thing on
    # this machine that can see outside the application folder, so the launcher
    # lists -- every workspace the start screen offers, not just the one it was
    # started on -- and the editor browses what it was given.
    cost.mark('settings + workspace list')

    # THE DIRECTORY TABLE: every workspace, and the folders inside it, so the
    # browser can show a folder row and step into it -- which is how the
    # .hpappdir folders under C:\DATA are reached.  The launcher is the only side
    # that CAN list a folder (the image has no directory API at all), so it lists
    # what it can and the image browses that; the budgets below are what keeps the
    # table inside the image's memory, and anything dropped is logged.
    dirs = []
    total = 0

    def add_dir(path, parent, depth=1):
        """list `path` into the table; returns its index, or -1 when full"""
        nonlocal total
        if len(dirs) >= CFG_MAX_DIRS:
            return -1
        me = len(dirs)
        dirs.append({'path': path, 'parent': parent, 'depth': depth,
                     'files': [], 'subdirs': []})
        for (name, size, isdir) in list_dir(path, dbg):
            if name in (CFG_FILE, LOG_TAIL, IMAGE_NAME, CLIP_TAIL):
                continue
            if total >= CFG_MAX_FILES:
                log('[list] %s: the file budget is full (%d entries)'
                    % (path, total))
                break
            if isdir:
                # the folder is a ROW here and a DIRECTORY of its own; its index
                # is filled in below, once it has one
                # NO_DIR (not 0): a row whose directory never got a slot must
                # not point at directory 0, which is the FIRST WORKSPACE (the
                # application folder) -- that is how every .hpappdir folder past
                # the budget came out as nbCode.hpappdir
                dirs[me]['files'].append({'name': name,
                                          'bytes': CFG_NO_DIR,
                                          'isdir': True, 'child': -1,
                                          'path': path + '\\' + name})
                dirs[me]['subdirs'].append(name)
            else:
                dirs[me]['files'].append({'name': name, 'bytes': size,
                                          'isdir': False})
            total += 1
        return me

    # A LISTING THAT FAILS MAY NOT STOP THE EDITOR.  What the user asked for is
    # the editor: this whole phase only fills in the folder table behind the
    # browser, and a folder that cannot be read, a firmware that answers something
    # unexpected or a bug in the loop below must not turn into a traceback instead
    # of a session.  Whatever goes wrong is written to the durable log with the
    # TYPES involved -- the one report of it was "'' object has no attribute
    # append" on a line whose receiver is a list here, and the next reader should
    # not have to reproduce that to find out what the object really was.
    try:
        for (label, wsdir) in workspaces[:CFG_MAX_WS]:
            i = add_dir(wsdir, None)
            if i < 0:
                break
            log('[run] %s: %d entr(ies)' % (wsdir, len(dirs[i]['files'])))

        # ... and the folders inside them, breadth first, to CFG_DIR_DEPTH.  The
        # entries of a directory have to be CONTIGUOUS in the table the C side
        # reads (it stores first+count), so a folder's row is written where its
        # parent's range is open and the folder's own contents are appended after
        # ALL the current ranges -- the launcher writes them in that order (see
        # build_config).
        #
        # AN INDEX WALK, NOT A QUEUE.  Every directory that is found is appended to
        # `dirs`, so the table IS the queue: walking `i` forward over it IS the
        # breadth first order, and `i` -- the entry being expanded -- is exactly the
        # parent index the child needs.  The queue this replaced held tuples of
        # (that same dict, depth) and looked the parent up again with
        # dirs.index(parent), which is a search over the WHOLE table comparing dicts
        # by value: it answered the first directory that happened to be equal, and
        # it is where the reported crash was.
        i = 0
        while i < len(dirs):
            parent = dirs[i]
            me = i
            i += 1
            if parent['depth'] > CFG_DIR_DEPTH:
                # a folder this deep stays a row that cannot be opened, and the
                # walk CARRIES ON with the rest of the table: the queue this
                # replaced broke out of the whole loop here, so one deep folder in
                # the FIRST workspace left every directory after it -- including
                # the other workspaces' folders -- as rows that could not be
                # opened.  The comparison is `>` because that is what the old
                # check meant for the item it popped (depth 1 and 2 are expanded,
                # so C:\DATA and the app folders inside it are browsable, which
                # is the depth the user actually walks).
                continue
            for f in parent['files']:
                if not f.get('isdir'):
                    continue
                if len(dirs) >= CFG_MAX_DIRS:
                    log('[list] %s: the directory budget is full (%d): %s stays '
                        'a row that cannot be opened'
                        % (parent['path'], len(dirs), f['name']))
                    break
                j = add_dir(f['path'], me, parent['depth'] + 1)
                if j < 0:
                    break
                f['child'] = j
                f['bytes'] = j
                log('[run] %s: %d entr(ies)'
                    % (dirs[j]['path'], len(dirs[j]['files'])))
    except Exception as e:
        alert('[list] the workspace listing stopped early: %r' % (e,))
        log('[list] at that point: %d dir(s) %s, workspaces %s, cwd %r'
            % (len(dirs), type(dirs).__name__, type(workspaces).__name__, cwd))

    for d in dirs:
        d.pop('subdirs', None)

    cost.mark('listing every workspace')
    cfg_bytes = build_config(settings, workspaces, dirs, cwd,
                             open_file=path or '',
                             logpath=log_device_path(appdir),
                             frames=frames)
    n = write_config(CFG_FILE, cfg_bytes)
    log('[run] launch file %s, %d B; %s (%d entries in %d dir(s))'
        % (CFG_FILE, n, path or cwd, total, len(dirs)))

    # A RESIDENT IMAGE IS REUSED.  The same file, still mapped, with its
    # relocations applied: re-entering it is the whole start-up cost, and it is
    # free.  (Anything that could have changed the file is caught by the size: a
    # rebuilt image is a different size in practice, and `main.unload()` forces a
    # reload when someone wants certainty.)
    loader = _resident.loader
    if (loader is not None and _resident.entry and loader.owned
            and loader.dbg is dbg and len(image) == _resident.size
            and loader.slot_armed() is False):
        entry = _resident.entry
        _resident.hits += 1
        log('[load] reusing the image already in memory (run %d of this session)'
            % (_resident.hits + 1))
    else:
        loader = ShellcodeElfLoader(dbg)
        entry = loader.load(image, appdir)
        if entry:
            _resident.loader = loader
            _resident.entry = entry
            _resident.ring = loader.ring
            _resident.size = len(image)
            _resident.loads += 1
    if not entry:
        alert('[load] FAILED')
        loader.unload()
        return None

    progress(4, 6, 'loading nbCode.elf into the firmware')
    cost.mark('image load / reuse')
    ring = loader.ring
    state = struct.unpack('<I', bytes(dbg.read_mem(ring + RING_COUNT_OFF,
                                                   4)))[0]
    cap = max_insn if max_insn else loader.max_insn
    log('[run] entering the image at 0x%08X (cap %d instructions)'
        % (entry, cap))
    try:
        dbg.max_insn = cap
    except Exception:
        pass

    # r0 = THE APPLICATION FOLDER, as a NUL-terminated UTF-8 device path, or 0.
    #
    # A string, not a struct: the image cannot work out where it was loaded from,
    # it gives the C side the path the FIRMWARE's fopen needs, and the two sides
    # then hold no shared memory layout at all (which is what the launch file
    # exists to avoid).  The image copies it during nb_run, so this block is
    # freed as soon as the call returns.
    arg = 0
    if appdir:
        b = utf8_bytes(appdir) + b'\x00'
        arg = dbg.call(ADDR_MALLOC, len(b))
        if arg:
            dbg.write_mem_bytes(arg, b)
            log('[run] app folder in r0: %s' % appdir)
        else:
            alert('[run] no memory for the app folder: the image will use its '
                'built-in path')
    try:
        ret = dbg.call(entry, arg, 0, 0, 0)
    except Exception as e:
        # the run could not be completed: print whatever the image managed to
        # say before the cap (this is the launcher's only view of a crash)
        drain_ring(dbg, ring, state)
        alert('[run] the editor did not return: %r' % (e,))
        # the slot is the launcher's job now (the image only disarms itself), and
        # a session that died is exactly when it must not be skipped
        stand_down_input(dbg, loader, image)
        loader.unload()
        return None
    finally:
        # the string went into the image's own buffer during nb_run
        if arg:
            try:
                dbg.call(ADDR_FREE, arg)
            except Exception:
                pass
    # the debug interface's return value is unreliable on this hardware
    # (primetcc measured it reporting 2 whatever main returns), so the status is
    # parsed out of the ring: the image writes "RT_RET:<n>" on the way out
    state = drain_ring(dbg, ring, state)
    code = parse_rt_ret(dbg, ring, state)
    if code is None:
        code = ret if isinstance(ret, int) else 0

    # the editor has returned (or died): hand the firmware's input slot back if
    # the image could not, because the launcher is about to free the memory the
    # hook points into
    progress(5, 6, 'starting the editor')
    cost.mark('the editor session')
    stand_down_input(dbg, loader, image)
    # KEEP THE IMAGE IF IT CAN BE KEPT: the next run re-enters it instead of
    # loading it again.  unload() holds it back when the OS is still inside the
    # hook, which is exactly the state it is in after a quit -- and `_resident`
    # then still points at it, so the next run reuses it (see Resident).
    loader.unload()
    if loader.owned and _resident.loader is loader:
        log('[load] the image stays mapped: the next run reuses it')
    progress(6, 6, 'closing down')
    cost.mark('teardown')
    cost.report()
    # THE LAST LINE OF A SESSION SAYS HOW IT WENT, in the console's own colour for
    # it (moreprint's p_pass / p_warning).  It is also the [exit] line the tests
    # and the durable log read, so nothing was lost by merging the two.
    if code == 0:
        # NOT FORCED: a session that worked is news in the log, not on the console
        # the user is looking at (see log() -- the console is quiet by default, and
        # `verbose` brings this line back in p_pass's colour)
        succeed('[exit] status 0: the editor returned to the launcher',
                force=verbose())
    else:
        note('[exit] status %d: the editor did not come back cleanly' % code)
    return code



def stand_down_input(dbg, loader, image):
    """Disarm the input hook and restore the firmware's own slot bytes.

    The IMAGE does this on its way out (nb_run_end, with the CP15 cache ritual);
    this is the net for a session that died inside the hook, and it matters: an OS
    whose get_event slot still points into a block we are about to free has a dead
    keyboard until the next reset.

    THREE THINGS ARE COPIED FROM primeLua, each learned on hardware:

      * THE STATE IS VALIDATED, not trusted: its magic has to be found in THIS
        image and its hook_fn has to be the address the slot currently holds.
        Without that check, a stale image's "saved original" would be written
        over a working slot.
      * THE BRANCH TARGET IS ZEROED FIRST.  Word 0 is the branch itself
        ("ldr pc, [pc, #-4]") and word 1 is where it goes, so while the trap is
        still in place the target is set to 0 and only then to its real value:
        the OS's input thread can dispatch at any moment, and a jump to 0 is a
        clean fault where a jump into a half-updated address is not.
      * THE FIRMWARE'S OWN STUB IS THE FALLBACK, word for word (nb_input.h).  NOPs
        would leave every application on the calculator without a keyboard.
    """
    try:
        slot = bytes(dbg.read_mem(FW_EVENT_SLOT, 8))
    except Exception:
        return
    if len(slot) < 8:
        return
    w0, hook_fn = struct.unpack('<II', slot)
    if w0 != SLOT_TRAP:
        return                  # the image already put the firmware's bytes back
    state = ElfTools.find_magic(image, b'NBINPUT', loader.image_base)
    if not state:
        alert('[hook] the slot holds our trap but the input state was not found in '
            'the image: leaving it alone rather than guessing')
        return
    their_fn = 0
    saved_valid = 0
    saved = None
    try:
        their_fn = struct.unpack('<I', bytes(dbg.read_mem(state + 8, 4)))[0]
        saved_valid = struct.unpack('<i', bytes(dbg.read_mem(state + 28, 4)))[0]
        if saved_valid:
            saved = bytes(dbg.read_mem(state + 12, 16))
    except Exception:
        saved = None
    if their_fn and their_fn != hook_fn:
        log('[hook] the state belongs to another image (hook_fn 0x%08X, the slot '
            'holds 0x%08X): leaving the slot alone' % (their_fn, hook_fn))
        return
    if not saved or len(saved) < 16 or struct.unpack('<I', saved[0:4])[0] == \
            SLOT_TRAP:
        saved = struct.pack('<IIII', 0xE52D0004, 0xE52DE004, 0xEF01003F,
                            0xE49D0004)
        why = "the firmware's own stub (no usable original was saved)"
    else:
        why = 'the bytes saved before the patch'
    words = list(struct.unpack('<IIII', saved[:16]))
    armed_now = w0 == SLOT_TRAP
    for i in ((1, 0, 2, 3) if armed_now else (0, 1, 2, 3)):
        try:
            if i == 1 and armed_now:
                dbg.write_mem(FW_EVENT_SLOT + 4, 0)     # target: nothing, first
            dbg.write_mem(FW_EVENT_SLOT + 4 * i, words[i])
        except Exception as e:
            alert('[hook] slot write %d failed: %r' % (i, e))
            return
    try:
        back = bytes(dbg.read_mem(FW_EVENT_SLOT, 4))
        ok = len(back) >= 4 and struct.unpack('<I', back)[0] != SLOT_TRAP
    except Exception:
        ok = False
    log('[hook] firmware input slot restored from %s: %s'
        % (why, 'ok' if ok else 'the write did not take'))


# ---------------------------------------------------------------------------
# the launcher's own screen: the workspace and file list
# ---------------------------------------------------------------------------
def label_of(path):
    """The last path component: "C:\\DATA\\notes" reads as "notes", which is what
    a user recognises in a list."""
    return path.rsplit('\\', 1)[-1] or path


def default_workspaces(appdir):
    """The workspaces to offer when setting.json does not name any: the
    application folder, C:\\DATA and C:\\, which is where a calculator's source
    files actually live.  A user's own list is `settings_workspaces` below."""
    out = []
    seen = set()
    for entry in (appdir, 'C:\\DATA', 'C:\\'):
        if entry and entry not in seen:
            seen.add(entry)
            out.append((label_of(entry), entry))
    return out


def settings_workspaces(settings, appdir):
    """setting.json's "workspaces", normalised, or the defaults.

    An entry is either a plain device path

        "workspaces": ["C:\\\\DATA\\\\mysrc"]

    or a [label, path] pair, which is what a user writes when the folder name is
    not the label they want to see:

        "workspaces": [["my sources", "C:\\\\DATA\\\\mysrc"]]

    Anything else is skipped rather than crashed on: this is a file a user edits
    by hand, and the launcher's job is to start the editor anyway.
    """
    listed = []
    for entry in (settings.get('workspaces') or []):
        path = None
        label = None
        if isinstance(entry, str) and entry:
            path = entry
        elif isinstance(entry, (list, tuple)) and len(entry) == 2:
            first, second = entry
            if isinstance(second, str) and second:
                path = second
                label = first if isinstance(first, str) and first else None
        if path:
            listed.append((label or label_of(path), path))
    if not listed:
        log('[settings] no usable "workspaces": offering the default folders')
        return default_workspaces(appdir)
    return listed


def probe(appdir=None, dbg=None):
    """The load chain, step by step, without starting the editor.  This is what
    to run on a real calculator when something is wrong: every step logs before
    it happens, so the last line in nbcode.log names the culprit.

    It does not run the image: the point is to find out whether the image WOULD
    run, so the last thing it does is load and free it.

    IT PRINTS, even though a normal start-up does not (see log()): this entry
    point exists to be read, and it is run by hand when something is broken.
    """
    global _verbose
    saved = _verbose
    set_verbose(True)
    try:
        dbg = dbg or PrimeDebug()
        appdir = appdir or find_app_dir(dbg)
        log('[probe] appdir %s' % appdir)
        log('[probe] a bare name reads: %s'
            % ('yes' if read_bytes('setting.json') is not None else 'no'))
        settings, why = read_settings(appdir)
        log('[probe] settings: %s' % why)
        log('[probe] workspaces: %r'
            % (settings_workspaces(settings, appdir),))
        log('[probe] listing %s: %d entr(ies)'
            % (appdir, len(list_dir(appdir, dbg))))
        image = read_image(appdir)
        if image is None:
            return 2
        log('[probe] debug channel open')
        loader = ShellcodeElfLoader(dbg)
        entry = loader.load(image, appdir)
        log('[probe] loader -> entry 0x%08X' % entry)
        if entry:
            log('[probe] ring 0x%08X, image base 0x%08X' % (loader.ring,
                                                            loader.image_base))
            loader.unload()
        else:
            log('[probe] the image did not load: see the [load] lines above')
        return 0 if entry else 3
    finally:
        _verbose = saved


def read_image(appdir=None):
    """The ELF's bytes, or None.  A bare name first: MicroPython resolves it
    against the application folder, and that is the only spelling it accepts."""
    raw = read_bytes(IMAGE_NAME, appdir)
    if raw is None:
        alert('[probe] cannot read %s (bare name and the application folders '
            'were tried)' % IMAGE_NAME)
    return raw


def unload():
    global _session
    if _session:
        _session.close()
        _session = None


APP_DIR = 'C:\\DATA\\nbCode.hpappdir'
SELFTEST_NO_EDITOR = False


def run(path=None, appdir=None, dbg=None):
    dbg = dbg or PrimeDebug()
    appdir = appdir or find_app_dir(dbg)
    settings, why = read_settings(appdir)
    workspaces = settings_workspaces(settings, appdir)
    ui = {
        'appdir': appdir,
        'settings': settings,
        'workspaces': workspaces,
        'cwd': workspaces[settings.get('default_workspace', 0) % len(workspaces)][1]
        if workspaces else 'C:\\',
    }
    if SELFTEST_NO_EDITOR:
        log('[selftest] settings=%s workspaces=%d' % (why, len(workspaces)))
        return 0
    return run_file(path, ui, dbg=dbg)


def main_menu(dbg=None):
    """The console menu: what the calculator's Python application runs when nbCode
    is started, and what `import main` does on the device."""
    dbg = dbg or PrimeDebug()
    appdir = find_app_dir(dbg)
    settings, why = read_settings(appdir)
    log('nbCode -- settings: %s' % why)
    workspaces = settings_workspaces(settings, appdir)
    for i, (label, path) in enumerate(workspaces):
        log('  %d) %-16s %s' % (i + 1, label, path))
    return run(dbg=dbg)


# ---------------------------------------------------------------------------
# the entry point
#
# NOT `if __name__ == '__main__'`.  The calculator runs this file from its Python
# application, where __name__ is not "__main__" the way CPython sets it, and
# `import main` from the shell has to work as well -- so the interpreter is what
# decides (primeLua's launcher does the same).  On a host nothing runs on import,
# which is what lets the tests load this file as a module.
# ---------------------------------------------------------------------------
if is_device():
    main_menu()
