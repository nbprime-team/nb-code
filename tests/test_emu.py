#!/usr/bin/env python3
"""test_emu.py -- nbCode's real image, loaded and driven the way the calculator
does it.

WHAT THIS COVERS THAT test_host.c DOES NOT
    test_host.c proves the editor's logic on the PC.  This proves the things
    that only exist on the device side of the port:

      * nbCode.elf is a well-formed image for the shellcode loader: an ELF whose
        relocations are all R_ARM_RELATIVE, with a "NBCORING" log ring and a
        "NBINPUT" input state inside its PT_LOAD segments;
      * the launcher (calc/main.py, the REAL file) can build the config block
        src/nb_cfg.h declares -- if the two ever drift apart the editor reads a
        pointer where a count belongs, and this test is what catches it first;
      * the entry point runs on the emulator as loaded (nb_entry -> nb_run ->
        the editor loop), arms the firmware's input hook, and returns;
      * a KEY EVENT injected into the emulator's firmware queue goes through the
        patched slot, into the image's hook, out of its queue and into the
        editor -- the whole chain, including the CP15 patch;
      * the launcher's stand-down restores the firmware's own get_event bytes,
        so freeing the image cannot leave a dead keyboard.

    The emulator is primetcc's (tests/emu_arm2.py), the same one primeLua's
    suites drive, with its documented defects patched by emu_fixes.py.

Run:  python3 tests/test_emu.py        (or `make test-emu`)
"""
import ast
import builtins
import contextlib
import importlib.util
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
PRIMETCC_TESTS = os.path.abspath(os.path.join(TOP, '..', 'primetcc', 'tests'))
PRIMELUA_TESTS = os.path.abspath(os.path.join(TOP, '..', 'primeLua', 'tests'))

# A sibling primetcc checkout wins over the vendored copy when there is one: the
# emulator is shared with the other projects in this family, and a fix there
# should show up here without anyone remembering to re-copy the file.  The
# vendored tests/emu_arm2.py is what makes the suite run in a nbCode checkout
# alone (which is the case on a build machine).
if os.path.exists(os.path.join(PRIMETCC_TESTS, 'emu_arm2.py')):
    sys.path.insert(0, PRIMETCC_TESTS)
sys.path.insert(0, HERE)

import emu_arm2                                      # noqa: E402
EMU_SRC = getattr(emu_arm2, '__file__', '?')
# primetcc's ARMv5 emulator has three documented defects (DSP multiplies, LDM
# with the base in the list, post-indexed LDRD/STRD) plus a wrong model of the
# firmware's get_event stub.  emu_fixes.py patches all four by monkey-patching;
# nbCode vendors primeLua's copy so this suite works without that checkout.
try:
    import emu_fixes                             # noqa: E402,F401
except ImportError:                              # pragma: no cover
    sys.path.insert(0, PRIMELUA_TESTS)
    import emu_fixes                             # noqa: E402,F401

from emu_arm2 import ARMEmu, BASE                    # noqa: E402

VERBOSE = '-v' in sys.argv
KEEP = '-k' in sys.argv
LOADED_STRIPPED = False          # keep the fake app folder (see main())
RESULTS = []

ELF = os.path.join(TOP, 'build', 'nbCode.hpappdir', 'nbCode.elf')
MAIN_PY = os.path.join(TOP, 'calc', 'main.py')
APP_NAME = 'nbCode.hpappdir'
DEVICE_DIR = 'C:\\DATA\\' + APP_NAME

SERVICE_TABLE = 0x307FBCAC
CFG_AREA_MAGIC = 0x45524150
ADDR_MALLOC = SERVICE_TABLE + 0xC * (0x10037 - 0x10000)
EVENT_SLOT = 0x307FBFA0
SLOT_TRAP = 0xE51FF004
FIRMWARE_STUB = (0xE52D0004, 0xE52DE004, 0xEF01003F, 0xE49D0004)

RING_MAGIC = b'NBCORING'
INPUT_MAGIC = b'NBINPUT'
# src/nb_clip.h: the global clipboard's file starts with this line
CLIP_MAGIC = b'NBCLIP1\n'


def check(name, ok, detail=''):
    RESULTS.append((name, ok, detail))
    print('%s %-58s %s' % ('ok  ' if ok else 'FAIL', name, detail))
    return ok


# ---------------------------------------------------------------------------
# mock MicroPython, backed by the debug device (the same shape primeLua's
# launcher tests use: the launcher is the REAL calc/main.py)
# ---------------------------------------------------------------------------
FAKE_ROOT = None


class DeviceBytes(bytes):
    """MicroPython's bytes, for the one thing that bit the launcher: THE CODEC.

    The calculator does not implement the non-strict error handlers.  There is
    no 'replace' and no 'ignore': `b'\\xff'.decode('utf-8', 'replace')` is a
    UnicodeError there whatever the data is, which is how the exit path died --
    the deployed launcher parsed the image's log ring (line 1359) with
    errors='replace', and a window that starts inside one of nbCode's Chinese
    messages is enough to abort it.

    Modelling that HERE, in the launcher's own globals, means every check in this
    suite runs against the device's rules instead of CPython's -- a launcher that
    passes a non-strict handler fails the suite instead of failing on hardware.
    """

    def decode(self, encoding='utf-8', errors='strict'):
        if errors != 'strict':
            raise UnicodeError('decode: errors=%r is not implemented'
                               % (errors,))
        return bytes.decode(self, encoding, errors)


def device_builtins():
    """The launcher's builtins with the calculator's bytes in place."""
    b = dict(vars(builtins))
    b['bytes'] = DeviceBytes
    return b


class cd:
    """context manager: run a block with the process's working directory set.

    MicroPython's file API is relative to the application directory, so a test
    that drives the launcher the way the device does has to BE in that directory
    -- `os.listdir('.')` is what the launcher's file listing does.
    """

    def __init__(self, path):
        self.path = path
        self.saved = None

    def __enter__(self):
        self.saved = os.getcwd()
        os.chdir(self.path)
        return self

    def __exit__(self, *a):
        if self.saved:
            os.chdir(self.saved)
        return False


class FakeFileIO:
    """uio.FileIO-alike, WITH THE DEVICE'S RESTRICTION ENFORCED.

    MicroPython here resolves a relative name against the application folder and
    REFUSES anything that looks like an absolute path ("C:\\DATA\\..." comes back
    as OSError 0 -- measured on hardware).  An earlier version of this mock
    stripped the application prefix and accepted those paths, which is exactly
    why the launcher's absolute-path open of nbCode.elf passed every test and
    failed on the calculator.  The rule is now the device's: bare names only.
    """

    def __init__(self, name, mode='rb'):
        self.name = name
        self.path = self.resolve(name)
        if 'w' in mode:
            self.f = open(self.path, 'wb+')
        elif 'a' in mode:
            self.f = open(self.path, 'ab+')
        elif '+' in mode:
            self.f = open(self.path, 'r+b')
        else:
            self.f = open(self.path, 'rb')

    @staticmethod
    def resolve(name):
        """THE DEVICE'S PATH RULES, on a host: a relative name lands in the
        application folder, an absolute one is an error."""
        norm = name.replace('/', '\\')
        if norm.startswith('\\') or (len(norm) > 1 and norm[1] == ':'):
            raise OSError('EINVAL: absolute paths are not supported')
        return os.path.join(FAKE_ROOT, norm.replace('\\', '/'))

    def read(self, n=-1):
        # the device's file API returns bytes, so this is where the codec model
        # has to start: what the launcher decodes is what this returns
        return DeviceBytes(self.f.read(n))

    def write(self, b):
        return self.f.write(bytes(b))

    def seek(self, off, whence=0):
        return self.f.seek(off, whence)

    def tell(self):
        return self.f.tell()

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()
        return False


class FakeDebug:
    """Plays the firmware's "debug" device on top of the emulator.

    A call into the service table is dispatched to the emulator's SVC backend; a
    call to anything else is ARM code to run -- which is how the entry point of
    nbCode.elf and its hook are entered, with the same "which live allocation
    contains this address" logic the real device does not need but a host test
    does.
    """

    def __init__(self, emu):
        self.emu = emu
        self.regions = {}
        self.entry_calls = 0
        # EVERY round trip to the firmware, counted: this is the cost that
        # matters on a calculator (a debug call is a device write plus a read),
        # and it is what the loader's fast paths exist to keep down.  See the
        # budget check in run_editor_check -- writing this image one word at a
        # time was ~124,000 of these and took minutes on hardware.
        self.round_trips = 0

    def read_mem(self, addr, size):
        self.round_trips += 1
        off = addr - BASE
        return DeviceBytes(self.emu.mem[off:off + size])

    def write_mem(self, addr, val):
        self.round_trips += 1
        self.emu.wr32(addr, val & 0xFFFFFFFF)

    def write_mem_bytes(self, addr, data):
        # one debug write per word: this is the slow path, on purpose
        self.round_trips += (len(data) + 3) // 4
        pad = (4 - (len(data) % 4)) % 4
        data = bytes(data) + b'\x00' * pad
        self.emu.mem[addr - BASE:addr - BASE + len(data)] = data

    def close(self):
        pass

    def call(self, fn, *args):
        emu = self.emu
        self.round_trips += 1
        if SERVICE_TABLE <= fn < SERVICE_TABLE + 0xC * 0x300:
            ident = 0x10000 + (fn - SERVICE_TABLE) // 0xC
            r = list(args) + [0] * (4 - len(args))
            emu.fw.call(ident, r, emu)
            if ident == 0x10037 and r[0]:
                self.regions[r[0]] = args[0]
            elif ident == 0x1003a and args and args[0]:
                self.regions.pop(args[0], None)
            return r[0]
        self.entry_calls += 1
        base = None
        for addr, size in self.regions.items():
            if addr <= fn < addr + size + 16:
                if base is None or size > self.regions[base]:
                    base = addr
        if base is None:
            base, entry = fn, 0
        else:
            size = self.regions[base]
            base = base if size <= 4096 else (base + 15) & ~7
            entry = fn - base
        arg0 = args[0] if args else 0
        # THE SECOND ARGUMENT MATTERS: the in-firmware loader takes
        # (path, image_base) in r0/r1 (see its disassembly in calc/main.py), and a
        # shim that dropped r1 loaded the image at address 0
        arg1 = args[1] if len(args) > 1 else 0
        # the LAUNCHER decides how long a run may take (loader.max_insn); the
        # emulator enforces it by raising, which this shim lets through so the
        # test sees "the editor overran" instead of hanging
        # the launcher decides how long a run may take (loader.max_insn); the
        # emulator enforces it by raising, so a hung editor fails the test
        # instead of hanging it
        # A BUDGET PER CALL, NOT FOR THE SESSION: the emulator's insn_count runs
        # for the whole test run (it is never reset), so comparing it against a
        # fixed cap made every later check fail once the session as a whole had
        # done that much work -- which is exactly what happened when the image
        # grew.  The intent is "this call may not run more than N instructions",
        # i.e. a hung editor fails the test instead of hanging it.
        emu.max_insn = emu.insn_count + getattr(self, 'max_insn',
                                                40 * 1000 * 1000)
        print('   [emu] entering the image at 0x%08X (cap %d insn)'
              % (base + entry, emu.max_insn), flush=True)
        before = emu.insn_count
        r = emu.run(entry, arg0, base=base, arg1=arg1)
        if VERBOSE:
            print('    [emu] entry returned %r after %d instructions'
                  % (r, emu.insn_count - before))
        return r


class FakeKeypad:
    """hpprime.keyboard(): a bit mask of the DEVICE key codes (nb_keymap.c's
    table), which is what the launcher would poll if it needed keys -- nbCode
    takes them through the input hook instead, so this exists only for the
    launcher's own screens and for completeness."""

    def __init__(self):
        self.down = 0

    def keyboard(self):
        return self.down


def make_uio_module():
    m = type(sys)('uio')
    m.FileIO = FakeFileIO
    return m


class FakeHpprime:
    def __init__(self):
        self.log = []
        self.keys = FakeKeypad()

    def keyboard(self):
        return self.keys.keyboard()

    def eval(self, *a):
        return 0

    def print(self, *a):
        self.log.append(' '.join(str(x) for x in a))

    def moreprint(self):
        return None


# the debug shim the launcher must use for THIS emulator.  run_file() builds its
# own PrimeDebug, so the class itself is patched (see load_main) -- otherwise the
# launcher would try to open a real "debug" device on the host and every run
# would fail before it started.
_DEBUG = None


def stripped_source():
    """calc/main.py with its prose removed, exactly as `make deploy` ships it."""
    import importlib.util as ilu
    spec = ilu.spec_from_file_location(
        'nbcode_strip', os.path.join(TOP, 'tools', 'strip_prose.py'))
    stripper = ilu.module_from_spec(spec)
    spec.loader.exec_module(stripper)
    return stripper.strip(io.open(MAIN_PY, encoding='utf-8').read())


def load_main(emu, hp, source=None, moreprint=None):
    """Import the launcher the way the calculator does.

    THE STRIPPED SOURCE, BY DEFAULT: that is the file `make deploy` puts on the
    calculator, and running the COMMENTED one here is how a stripper bug shipped
    (a multi-line log message lost its second half, and the deployed launcher died
    with "not all arguments converted during string formatting" on the exit path
    that this suite uses on every run).  `source` is there for the comparison
    checks, which need both.
    """
    global _DEBUG, LOADED_STRIPPED
    fake_debug = FakeDebug(emu)
    _DEBUG = fake_debug
    if source is None:
        source = stripped_source()
        LOADED_STRIPPED = True
    else:
        LOADED_STRIPPED = False
    import types as _types
    mod = _types.ModuleType('nbcode_launcher')
    mod.__file__ = MAIN_PY
    # THE DEVICE'S BUILTINS (DeviceBytes): the launcher only ever sees the
    # calculator's codec, so a non-strict error handler is an error here too
    mod.__dict__['__builtins__'] = device_builtins()
    saved = {k: sys.modules.get(k) for k in ('uio', 'hpprime', 'moreprint')}
    sys.modules['uio'] = make_uio_module()
    sys.modules['hpprime'] = hp
    # A STAND-IN FOR THE USER'S moreprint.py (see moreprint_check): the launcher
    # imports it if it is there, so the test has to be able to put one there.
    if moreprint is not None:
        sys.modules['moreprint'] = moreprint
    else:
        sys.modules.pop('moreprint', None)
    try:
        exec(compile(source, MAIN_PY + ' (deployed)', 'exec'), mod.__dict__)
    finally:
        for k, v in saved.items():
            if v is None:
                sys.modules.pop(k, None)
            else:
                sys.modules[k] = v
    # the launcher's PrimeDebug speaks the debug FILE protocol; on the host the
    # file is the emulator itself
    mod.PrimeDebug = lambda *a, **kw: _DEBUG
    return mod, fake_debug


def setup_tree():
    """a fake C:\\DATA\\nbCode.hpappdir with the image and a source file."""
    global FAKE_ROOT
    root = tempfile.mkdtemp(prefix='nbcode_emu_')
    app = os.path.join(root, APP_NAME)
    os.makedirs(app)
    shutil.copy(ELF, os.path.join(app, 'nbCode.elf'))
    with open(os.path.join(app, 'setting.json'), 'w') as f:
        f.write('{"theme": 0, "tab_width": 4, "highlight": true}\n')
    with open(os.path.join(app, 'hello.c'), 'w') as f:
        f.write('int main(void) {\n    return 0;\n}\n')
    with open(os.path.join(app, 'notes.txt'), 'w') as f:
        f.write('a note\n')
    FAKE_ROOT = app
    emu_arm2.FAKE_ROOT = app
    return root, app


def read_elf():
    with open(ELF, 'rb') as f:
        return f.read()


# ---------------------------------------------------------------------------
# the checks
# ---------------------------------------------------------------------------
def image_checks(tag):
    """Everything about the IMAGE that the loader depends on, checked from the
    file rather than from the emulator: this is what `make` has to keep true."""
    data = read_elf()
    check('%s: it is an ELF32 ARM image' % tag,
          data[0:4] == b'\x7fELF' and struct.unpack_from('<H', data, 18)[0] == 40,
          'machine=%d' % struct.unpack_from('<H', data, 18)[0])

    # relocations: the loader applies R_ARM_RELATIVE only
    phoff = struct.unpack_from('<I', data, 28)[0]
    phent = struct.unpack_from('<H', data, 42)[0]
    phnum = struct.unpack_from('<H', data, 44)[0]
    dyn = None
    loads = []
    for i in range(phnum):
        off = phoff + i * phent
        fields = struct.unpack_from('<IIIIIIII', data, off)
        if fields[0] == 1:
            loads.append((fields[1], fields[2], fields[4], fields[5]))
        elif fields[0] == 2:
            dyn = fields[1]
    kinds = {}
    if dyn:
        tags = {}
        off = dyn
        while True:
            t, v = struct.unpack_from('<II', data, off)
            if t == 0:
                break
            tags[t] = v
            off += 8
        rel, relsz = tags.get(17), tags.get(18)
        if rel and relsz:
            for i in range(relsz // 8):
                r_off, info = struct.unpack_from('<II', data, rel + i * 8)
                kinds[info & 0xFF] = kinds.get(info & 0xFF, 0) + 1
    check('%s: every relocation is R_ARM_RELATIVE' % tag,
          set(kinds.keys()) <= {23}, str(kinds))
    check('%s: the image has PT_LOAD segments' % tag, bool(loads),
          '%d segments, %d B of file bytes'
          % (len(loads), sum(s[2] for s in loads)))

    # the two magics the launcher and the tests look for must be inside a
    # PT_LOAD segment, or find_magic cannot compute their address
    for (magic, what) in ((RING_MAGIC, 'log ring'), (INPUT_MAGIC, 'input state')):
        idx = data.find(magic)
        inside = idx >= 0 and any(o <= idx < o + fs for (o, _v, fs, _m) in loads)
        check('%s: the %s is in a loaded segment' % (tag, what), inside,
              'offset 0x%X' % idx if idx >= 0 else 'not found')
    return data


def launcher_config_check(data):
    """The launcher's settings and file listing: setting.json is parsed, and the
    file list it hands the editor is the one the directory module returns."""
    root, app = setup_tree()
    emu = ARMEmu()
    hp = FakeHpprime()
    mod, dbg = load_main(emu, hp)

    with cd(app):
        settings, why = mod.read_settings(DEVICE_DIR)
    check('launcher: setting.json parsed', why == 'ok', why)
    check('launcher: the settings arrived',
          settings.get('tab_width') == 4 and settings.get('highlight') is True,
          str({k: settings[k] for k in ('theme', 'tab_width', 'highlight')}))

    files = [{'name': 'hello.c', 'bytes': 24, 'isdir': False},
             {'name': 'notes.txt', 'bytes': 7, 'isdir': False}]
    workspaces = [('nbCode.hpappdir', DEVICE_DIR)]

    # THE START SCREEN'S LIST, which is the one thing in setting.json a user
    # writes by hand.  A bare path and a [label, path] pair both have to work,
    # and garbage has to fall back to the defaults instead of raising: the
    # launcher's job is to start the editor anyway.
    def ws(entries, appdir=DEVICE_DIR):
        return mod.settings_workspaces({'workspaces': entries}, appdir)

    got = ws([])
    check('launcher: no "workspaces" falls back to the default folders',
          len(got) >= 3 and got[0][1] == DEVICE_DIR, repr(got[:3]))
    got = ws([r'C:\DATA\mysrc'])
    check('launcher: a bare path becomes a workspace, labelled by its last part',
          got == [('mysrc', r'C:\DATA\mysrc')], repr(got))
    got = ws([['my sources', r'C:\DATA\mysrc']])
    check('launcher: a [label, path] pair keeps the label the user chose',
          got == [('my sources', r'C:\DATA\mysrc')], repr(got))
    got = ws([['label only'], 42, None, ''])
    check('launcher: unusable entries are skipped, not fatal',
          len(got) >= 3 and got[0][1] == DEVICE_DIR, repr(got[:3]))
    got = ws([r'C:\DATA\a', ['b', r'C:\DATA\b']])
    check('launcher: the list keeps the order the user wrote',
          [p for _, p in got] == [r'C:\DATA\a', r'C:\DATA\b'], repr(got))

    # THE CONFIG'S LAYOUT IS CHECKED IN test_host.c (nb_cfg.h's offsets, against
    # a modelled buffer) and here by the run below (the editor either reads the
    # config the launcher wrote or it does not).  What this file checks about the
    # launcher is the part only the launcher can get wrong: the settings it
    # parses, the workspace list it offers and the file list it builds.
    return root, app, emu, mod, dbg


def run_editor_check(root, app, emu, mod, dbg, data):
    """Run the editor exactly the way the calculator does: the REAL launcher
    (calc/main.py's run_file), against the real nbCode.elf, for a bounded number
    of frames, with one key event waiting in the firmware's queue.

    Everything after the run is checked from the OUTSIDE -- the launcher's own
    log, the firmware's input slot, the file the image wrote -- rather than from
    anything the image says about itself.
    """
    settings, _ = mod.read_settings(app)
    workspaces = [('nbCode.hpappdir', DEVICE_DIR)]
    frames = 60
    # nbcode.log is appended to, so everything this run says starts where the file
    # ends NOW: that keeps the assertions below about THIS run, not about the
    # lines the earlier checks left there
    logpath = os.path.join(app, 'nbcode.log')
    log_before = os.path.getsize(logpath) if os.path.exists(logpath) else 0
    trips_before = _DEBUG.round_trips

    # ONE KEY, waiting before the editor starts: it has to travel through the
    # patched firmware slot, the image's hook and into the editor's queue
    emu.fw.inject_key(0x51, True)          # the 7/Q key
    emu.fw.inject_key(0x51, False)

    # WHERE THE IMAGE WILL LOOK FOR ITS LAUNCH FILE.  The constant is
    # "C:\\DATA\\nbCode.hpappdir" -- the device's application folder, which is
    # also where MicroPython's relative file access lands -- so the image reads
    # it as a relative name.  A host harness has no such folder, so the image's
    # build-time path is overridden to be relative to wherever the mock file API
    # resolves names.  (nb_cfg_dir is a weak symbol: nothing on the device
    # defines it.)
    # (the image's DIR is the app folder; a host has to say so itself)
    # a low instruction cap: a hung editor must FAIL this test, not hang it
    # A BOUNDED RUN.  The frame budget in the launch file is what normally ends
    # a session (12 frames here); this instruction cap is the belt to that
    # braces, so a hung editor fails the test instead of hanging it.
    mod.ShellcodeElfLoader.max_insn = 6 * 1000 * 1000
    _DEBUG.max_insn = 6 * 1000 * 1000
    # THE LOAD BASE IS RECORDED, NOT ASSUMED.  Where the firmware heap puts the
    # image depends on everything allocated before it -- and this suite allocates
    # in between (the loader's own tests) -- so it is spied on here.  An earlier
    # version of this file hardcoded 0x31000008, which was true until something
    # else allocated first; then the ring was read from the wrong address and two
    # checks "passed" against binary garbage.
    loads = []
    orig_load = mod.ShellcodeElfLoader.load

    def spy_load(self, data, appdir=None):
        entry = orig_load(self, data, appdir)
        if entry:
            loads.append((self.image_base, self.ring, entry))
        return entry

    mod.ShellcodeElfLoader.load = spy_load
    t0 = time.time()
    print('   [test] cwd=%s' % os.getcwd(), flush=True)
    try:
        console = io.StringIO()
        with cd(app):
            print('   [test] cwd in run=%s, files=%s'
                  % (os.getcwd(), sorted(os.listdir('.'))[:6]), flush=True)
            with contextlib.redirect_stdout(console):
                status = mod.run_file(DEVICE_DIR + '\\hello.c',
                                      {'appdir': DEVICE_DIR,
                                       'settings': settings,
                                       'workspaces': workspaces,
                                       'cwd': DEVICE_DIR},
                                      frames=frames,
                                      max_insn=6 * 1000 * 1000)
    finally:
        mod.ShellcodeElfLoader.load = orig_load
    # A NORMAL SESSION SAYS NOTHING (see console_quiet_check): everything the
    # launcher logged is in nbcode.log, and the console stays out of the user's
    # way.  The harness's own "[emu] entering the image" line is not the
    # launcher's and is allowed through.
    spoken = [ln for ln in console.getvalue().split('\n')
              if ln.startswith(('[load]', '[hook]', '[cost]', '[settings]',
                                '[exit]', '[run]', '[nbCode]', 'cfg',
                                '[list]', '[probe]'))]
    check('run: the launcher printed nothing on the way in or out',
          not spoken, repr(spoken[:3]))
    dt = time.time() - t0
    trips = _DEBUG.round_trips - trips_before
    check('run: the whole session stays under a debug-traffic budget',
          trips < 8000, '%d round trips in %.1fs' % (trips, dt))
    if VERBOSE:
        print('    [test] %d firmware round trips for the session' % trips)
    check('run: this suite drove the STRIPPED launcher that ships',
          LOADED_STRIPPED, '')
    check('run: the launcher ran the editor to a status', status is not None,
          'status %r after %.1fs' % (status, dt))
    check('run: the editor exited cleanly', status == 0, 'status %r' % (status,))

    # the input slot: the launcher's stand-down must have handed the firmware's
    # own bytes back, or the calculator has a dead keyboard until a reset
    words = struct.unpack('<IIII', bytes(dbg.read_mem(EVENT_SLOT, 16)))
    check('run: the input slot holds the firmware stub again (not our trap)',
          words[0] != SLOT_TRAP, '%08X %08X' % (words[0], words[1]))
    check('run: ... and it is the firmware stub word for word',
          words == FIRMWARE_STUB, '%08X %08X %08X %08X' % words)

    # THE PICTURE, IN THE FIRMWARE'S OWN FRAMEBUFFER.
    #
    # THIS IS THE CHECK THAT WAS MISSING.  nbCode draws straight into the buffer
    # the firmware hands it, and it once read that pointer wrong (it took the
    # LCD INFO structure's address as the pixels).  On the emulator the wrong
    # pointer lands 64 bytes before the real framebuffer, so the editor still
    # drew a picture -- 16 pixels off, over the info structure -- and every check
    # in this file passed while the calculator REBOOTED on the first frame,
    # because there the same wrong pointer points into firmware.
    #
    # So: the framebuffer must have been drawn into, and the LCD info structure
    # must be exactly as the firmware left it.
    fw = emu.fw
    if not hasattr(fw, 'fb_addr'):
        fw.setup_lcd()
    fb_lo = fw.fb_addr - emu.base
    pixels = bytes(emu.mem[fb_lo:fb_lo + 320 * 240 * 4])
    nonblank = sum(1 for i in range(0, len(pixels), 4)
                   if pixels[i:i + 4] != b'\x00\x00\x00\x00')
    check('run: the editor drew into the firmware framebuffer',
          nonblank > 1000, '%d non-black pixels of 76800' % nonblank)
    info = fw.fb_addr - 0x40 - emu.base
    w = struct.unpack_from('<H', emu.mem, info + 2)[0]
    h = struct.unpack_from('<H', emu.mem, info + 4)[0]
    fb_word = struct.unpack_from('<I', emu.mem, info + 16)[0]
    check('run: the firmware LCD structure is untouched', 
          (w, h, fb_word) == (320, 240, fw.fb_addr),
          'w=%d h=%d fb=0x%08X' % (w, h, fb_word))
    row0 = pixels[:320 * 4]
    check('run: ... and the drawn picture starts at the framebuffer, '
          'not 16 pixels late',
          row0.count(b'\x00\x00\x00\x00') < 320 * 4,
          repr(row0[:16]))

    # THE IMAGE'S OWN LOG, out of its ring.  This is the console a user reads on
    # the device (the launcher streams it while the editor runs), so the test
    # reads it after the fact and asserts on what it says.
    check('run: the loader reported where it put the image and the ring',
          len(loads) == 1, '%d load(s)' % len(loads))
    base, ring, _entry = loads[-1] if loads else (0, 0, 0)
    # the ring is INSIDE THE LOADED IMAGE: base + the segment addresses, which
    # is the MEMORY size, not the file size (the image's .bss is bigger than the
    # file it came from)
    mem_size = mod.ElfTools.memory_size(data)
    check('run: the ring address is inside the loaded image',
          base and base <= ring < base + mem_size, 'base=0x%08X ring=0x%08X'
          % (base, ring))
    count = struct.unpack('<I', bytes(dbg.read_mem(ring + 8, 4)))[0]
    n = min(count, 8192)
    start = (count - n) % 8192
    text = bytes(dbg.read_mem(ring + 12 + start, n)).decode('utf-8', 'replace')
    check('run: the image logged through the ring', 'nbCode' in text,
          repr(text[:80]))
    check('run: the launcher frame budget reached the image',
          'frame budget=60' in text, repr(text[:200]))
    # (the file count is whatever this suite left in the folder: the loader
    # tests write a tiny ELF there, so the assertion is on the settings that
    # came from the launcher, not on a number the suite itself moves)
    check('run: the launch file was parsed (theme, tab, workspaces, files)',
          'cfg: workspaces=1' in text and 'cfg: files=' in text
          and 'cfg: tab=4' in text, '')
    # THE r0 HANDOFF: MicroPython's file API refuses absolute paths and the
    # firmware's fopen wants them, so the launcher tells the image where the
    # application folder is -- and the image has to have used it, or it would be
    # reading its launch file with a spelling its own file API rejects
    check('run: the image got the app folder the launcher handed it',
          'app folder from the launcher' in text, '')
    check('run: ... and read its launch file by device path',
          'cfg: read by device path' in text, '')
    check('run: the editor opened the file the launcher named',
          '已打开 hello.c' in text, repr(text[-200:]))
    check("run: the editor's frame loop ran", 'frame count=16' in text, '')
    check('run: the exit status was published as RT_RET',
          'RT_RET:' in text, repr(text[-40:]))

    # the durable log the image writes through the firmware's own file API is
    # the LAUNCHER's log (the image's lines go through it too, as "LOG:").  The
    # launcher's own [load] lines are in it, which is where the segment copy is
    # visible: NOT in the ring, which is the image's console.
    check('run: the launcher wrote its log file', os.path.exists(logpath), '')
    launcher_log = ''
    if os.path.exists(logpath):
        with open(logpath) as f:
            f.seek(log_before)
            launcher_log = f.read()
    # THE CHEAPEST LOADER WON: the in-firmware blob (a handful of round trips),
    # NOT "one word per round trip" -- which is 121,875 of them for this image and
    # is what a start-up measured on the calculator before the blob was used.
    check('run: the in-firmware loader loaded the image',
          'loaded by the in-firmware loader' in launcher_log
          and 'copied from Python' not in launcher_log,
          repr([ln for ln in launcher_log.split('\n')
                if 'loader' in ln or 'copied' in ln][:3]))
    load_cost = [ln for ln in launcher_log.split('\n')
                 if 'image load / reuse' in ln]
    check('run: the first load costs hundreds of round trips, not 100,000',
          bool(load_cost) and int(load_cost[-1].split()[-3]) < 300,
          repr(load_cost[-1:]))
    # MILESTONES, for the crashes that never return: the run is ONE blocking
    # debug call, so a fault means the launcher never reads the ring -- the
    # durable log is then the only place a trail can survive, and this is what
    # turns "it rebooted after nbCode 1.0 start" into "it died at X".
    want = ('milestone: screen ready', 'milestone: ui ready',
            'milestone: input hook armed',
            'milestone: first frame about to be drawn')
    missing = [m for m in want if m not in launcher_log]
    check('run: the image left milestones in the durable log', not missing,
          'missing %r' % (missing,))
    # the hook's flight counter came back to zero, so the image could be freed:
    # this is the check that would have been "keeping the image mapped" forever
    # the IMAGE no longer writes the slot (that rebooted the calculator when it
    # did it at the end of a session): it disarms, and the launcher puts the
    # firmware's bytes back -- so this line is the proof the hand-back happened
    check('run: the launcher put the firmware slot back after the run',
          'input slot restored' in launcher_log, repr(
              [ln for ln in launcher_log.split('\n') if 'slot' in ln][-2:]))
    check('run: the session left the image resident for the next run',
          ('stays mapped' in launcher_log or 'keeping the image' in launcher_log)
          and 'given back' not in launcher_log,
          repr([ln for ln in launcher_log.split('\n') if 'image' in ln][-2:]))
    check('run: ... and the start-up cost was counted per phase',
          '[cost] start-up' in launcher_log,
          repr([ln for ln in launcher_log.split('\n') if '[cost]' in ln][-2:]))
    if VERBOSE:
        for line in text.split('\n'):
            if line:
                print('    | ' + line)


def device_constraints_check(app, mod):
    """THE THINGS THE CALCULATOR DOES NOT HAVE.

    Every one of these was a real failure on hardware before it was a test:

      * no `struct` module -- it is called `ustruct` there;
      * no `time` module at all;
      * no `json`/`ujson`, so setting.json needs the launcher's own parser;
      * no `__name__ == '__main__'` entry point (it is not how the Python
        application runs the file, and `import main` has to work too);
      * no absolute paths in MicroPython's file API -- "C:\\DATA\\nbCode.elf"
        comes back as OSError 0, which is what the launcher died on, and
        FakeFileIO now enforces that rule for every open in this suite.
    """
    src = open(MAIN_PY).read()
    check('device: the launcher imports ustruct, not struct',
          'import ustruct as struct' in src
          and '\nimport struct\n' not in src, '')
    check('device: ... and never imports the time module',
          'import time' not in src, '')
    # The source MENTIONS __name__ in prose (the comments and docstrings explain
    # why it is not used), so this is checked against the PARSE TREE: no code may
    # read it at all.
    import ast
    names = [n for n in ast.walk(ast.parse(src))
             if isinstance(n, ast.Name) and n.id == '__name__']
    check('device: ... and never reads __name__ in code',
          not names, '%d reference(s)' % len(names))
    check('device: ... and opens the debug device the way primetcc does',
          "_uio.FileIO('debug')" in src and "open('debug', 'rb')" in src, '')
    check('device: this host is not mistaken for the calculator',
          mod.is_device() is False, 'is_device()=%r' % (mod.is_device(),))

    # setting.json WITHOUT a json module: the calculator's case.  The launcher's
    # own parser has to agree with the platform's, or the settings silently
    # become the defaults on the device (which is what used to happen).
    text = ('{\n  "theme": 1,\n  "tab_width": 8,\n'
            '  "workspaces": [["src", "C:\\\\DATA\\\\src"], "C:\\\\DATA"],\n'
            '  "default_workspace": 0,\n  "highlight": true,\n'
            '  "readonly": false\n}\n')
    ours = mod.parse_json(text)
    check('device: the launcher parses setting.json without a json module',
          ours.get('theme') == 1 and ours.get('tab_width') == 8
          and ours.get('workspaces') == [['src', 'C:\\DATA\\src'],
                                         'C:\\DATA'], repr(ours))
    if json is not None:
        check('device: ... and agrees with the platform json module',
              ours == json.loads(text), '')
        saved = mod.json
        mod.json = None                     # what the calculator looks like
        try:
            with cd(app):
                got, why = mod.read_settings(DEVICE_DIR)
        finally:
            mod.json = saved
        check('device: ... so the settings still load with json=None',
              why == 'ok' and got.get('theme') == 0
              and got.get('tab_width') == 4, '%s %r' % (why, got.get('theme')))
    # WHERE THE APP FOLDER IS, and that the answer is validated rather than
    # believed: the firmware's cwd only counts when nbCode.elf is in it, and with
    # no answer at all the build-time constant is the fallback.
    with cd(app):
        found = mod.find_app_dir(_DEBUG)
    check('device: the app folder is found next to the image',
          os.path.basename(found) == APP_NAME, found)
    other = tempfile.mkdtemp(prefix='nbcode_elsewhere_')
    try:
        with cd(other):
            fallback = mod.find_app_dir(None)
    finally:
        shutil.rmtree(other, ignore_errors=True)
    check('device: ... and falls back to the built-in path when it is not',
          fallback == mod.APP_DIR, fallback)

    # THE FIRMWARE FINDER'S ANSWER: 40 bytes, and the fields the file list needs
    # are at fixed offsets.  Parsing is the part of the device listing that CAN
    # be tested on a host, so it is.
    raw = bytearray(40)
    struct.pack_into('<I', raw, mod.FIND_LFN_OFF, 0x1234)
    struct.pack_into('<I', raw, mod.FIND_SFN_OFF, 0x5678)
    struct.pack_into('<I', raw, mod.FIND_SIZE_OFF, 1234)
    raw[mod.FIND_ATTR_OFF] = mod.FIND_ATTR_DIR
    ent = mod.parse_find_entry(bytes(raw))
    check('device: the firmware finder answer is parsed at the right offsets',
          ent == {'lfn': 0x1234, 'sfn': 0x5678, 'size': 1234,
                  'attr': mod.FIND_ATTR_DIR}, repr(ent))

    for bad in ('{', '{"a": }', '{"a": 1} trailing', '[]'):
        try:
            got = mod.parse_json(bad)
            ok = not isinstance(got, dict) or bad == '[]'
        except Exception:
            ok = True
        check('device: malformed settings are rejected, not guessed (%r)'
              % (bad,), ok, '')
    if VERBOSE:
        print('    [test] parse_json(%r) = %r' % (text[:24], ours))



def stripped_launcher_check(app, mod):
    """THE FILE THAT SHIPS: main.py without its prose.

    The calculator compiles an imported module's whole source into RAM, so `make
    deploy` runs tools/strip_prose.py over the launcher (42% of it is comments
    and docstrings).  A stripper that removes one byte too many would break the
    device in a way no other check here could see -- every other test loads the
    COMMENTED source -- so the stripped copy is loaded as a module of its own and
    made to agree with the original.
    """
    import importlib.util as ilu
    spec = ilu.spec_from_file_location(
        'nbcode_strip', os.path.join(TOP, 'tools', 'strip_prose.py'))
    stripper = ilu.module_from_spec(spec)
    spec.loader.exec_module(stripper)
    source = io.open(MAIN_PY).read()
    stripped = stripper.strip(source)
    check('strip: it removes a substantial share of the launcher',
          len(stripped) < len(source) * 0.8,
          '%d B -> %d B' % (len(source), len(stripped)))
    try:
        compile(stripped, 'stripped', 'exec')
        ok = True
    except SyntaxError as e:
        ok = False
        print('    [test] stripped syntax error: %s' % (e,))
    check('strip: the result parses', ok, '')
    doc = stripped.lstrip()[:3]
    check('strip: the module docstring survives (it is the on-device help)',
          doc in (chr(34) * 3, chr(39) * 3), repr(stripped[:40]))

    m = types.ModuleType('nbcode_stripped')
    m.__dict__['__name__'] = 'nbcode_stripped'
    m.__dict__['__builtins__'] = device_builtins()
    exec(compile(stripped, 'stripped', 'exec'), m.__dict__)
    names = ('CFG_MAGIC', 'CFG_VERSION', 'CFG_MAX_WS', 'CFG_MAX_FILES',
             'CFG_FILE', 'IMAGE_NAME', 'LOG_TAIL', 'RING_MAGIC',
             'RING_COUNT_OFF', 'DEFAULTS', 'LANG_NAMES', 'APP_DIR')
    same = [k for k in names if getattr(m, k, 'x') == getattr(mod, k, 'y')]
    check('strip: every constant survived it', len(same) == len(names),
          '%d/%d, missing %r' % (len(same), len(names),
                                 [k for k in names if k not in same]))
    dirs = [{'path': 'C:\\DATA', 'parent': None,
             'files': [{'name': 'a.c', 'bytes': 12, 'isdir': False}]}]
    one = mod.build_config(mod.DEFAULTS, [('l', 'C:\\DATA')], dirs,
                           'C:\\DATA')
    two = m.build_config(m.DEFAULTS, [('l', 'C:\\DATA')], dirs, 'C:\\DATA')
    check('strip: the launch file it builds is byte for byte the same',
          one == two, '%r vs %r' % (one[:16], two[:16]))
    text = '{"tab_width": 2, "workspaces": [["x", "C:\\\\D"]]}'
    check('strip: ... and so is the settings parser it carries',
          m.parse_json(text) == mod.parse_json(text), '')
    check('strip: ... and it still does not treat this host as the calculator',
          m.is_device() is False, '')

    # EVERY LITERAL THAT IS NOT A DOCSTRING MUST SURVIVE.  Docstrings are what
    # stripping is FOR; any other string that goes missing is prose the stripper
    # mistook for one -- which is how the deployed launcher lost the second half
    # of a two-line log message and then died formatting it.
    def keep_strings(tree):
        drop = set()
        for node in ast.walk(tree):
            body = getattr(node, 'body', None)
            if isinstance(body, list) and body and isinstance(body[0], ast.Expr) \
                    and isinstance(body[0].value, ast.Constant) \
                    and isinstance(body[0].value.value, str):
                drop.add(id(body[0].value))
        return [n.value for n in ast.walk(tree)
                if isinstance(n, ast.Constant) and isinstance(n.value, str)
                and id(n) not in drop]

    want = keep_strings(ast.parse(source))
    have = keep_strings(ast.parse(stripped))
    missing = [x for x in want if x not in have]
    check('strip: no string literal but a docstring is dropped',
          not missing, '%d missing, first: %r' % (len(missing),
                                                  (missing[:1] or [''])[0][:60]))


class FakeMoreprint:
    """Stands in for the user's moreprint.py: records every coloured line.

    primeLua and primetcc print through this module, so a user who has one expects
    nbCode's console output to come out the same way -- and expects the SAME
    module to work for all three (see moreprint_check).
    """

    def __init__(self):
        self.lines = []

    def module(self):
        m = type(sys)('moreprint')
        for name in ('p_log', 'p_warning', 'p_error', 'p_pass', 'p_out'):
            setattr(m, name, self._record(name))
        return m

    def _record(self, name):
        def f(text):
            self.lines.append((name, text))
        return f

    def level(self, name):
        return [t for (n, t) in self.lines if n == name]


def workspace_walk_check(app, mod, emu):
    """THE NESTED FOLDER WALK: a workspace, the folders in it, and theirs.

    The launcher is the only side that can list a folder (the image has no
    directory API), so the browser's folder rows and the editor's ability to step
    into them come from this walk.  It is also the code whose queue the reported
    crash was in, and the reason it is checked HERE and not only on hardware is
    that a device path under the application folder is now resolvable on a host
    (see list_dir_os), which is what makes a nested fixture possible at all.
    """
    sub = os.path.join(app, 'sub')
    deep = os.path.join(sub, 'deep')
    deeper = os.path.join(deep, 'deeper')
    os.makedirs(deeper)
    with open(os.path.join(sub, 'in.c'), 'w') as f:
        f.write('int sub(void) { return 1; }\n')
    with open(os.path.join(deep, 'deeper.c'), 'w') as f:
        f.write('int deeper(void) { return 2; }\n')
    with open(os.path.join(deeper, 'x.c'), 'w') as f:
        f.write('int x(void) { return 3; }\n')
    logpath = os.path.join(app, 'nbcode.log')
    if os.path.exists(logpath):
        os.remove(logpath)
    settings, _ = mod.read_settings(app)
    with cd(app):
        status = mod.run_file(DEVICE_DIR + '\\hello.c',
                              {'appdir': DEVICE_DIR, 'settings': settings,
                               'workspaces': [('app', DEVICE_DIR)],
                               'cwd': DEVICE_DIR},
                              frames=4, max_insn=6 * 1000 * 1000)
    text = ''
    if os.path.exists(logpath):
        with open(logpath) as f:
            text = f.read()
    listed = [ln for ln in text.split('\n') if ln.startswith('[run] ') and
              'entr(ies)' in ln]
    check('walk: the workspace and the folder inside it were both listed',
          len(listed) >= 2 and
          any(l.startswith('[run] ' + DEVICE_DIR + '\\sub:') for l in listed),
          repr(listed[:3]))
    check('walk: ... and the folder inside THAT one is a row of its own',
          any(l.startswith('[run] ' + DEVICE_DIR + '\\sub\\deep:') and
              not l.endswith(': 0 entr(ies)') for l in listed),
          repr([l for l in listed if 'deep' in l][:2]))
    check('walk: ... while the fourth level stays out of the walk',
          not any(DEVICE_DIR + '\\sub\\deep\\deeper' in l for l in listed),
          repr([l for l in listed if 'deeper' in l][:2]))
    check('walk: the run itself was unaffected by the walk',
          status == 0, 'status %r' % (status,))
    check('walk: the folder row reached the launch file',
          os.path.exists(os.path.join(app, 'nbcode.cfg')) and
          b'sub' in open(os.path.join(app, 'nbcode.cfg'), 'rb').read(),
          '')
    shutil.rmtree(sub, ignore_errors=True)


def finder_masks_check():
    """THE FILE LIST ASKS WITH EVERY FINDER MASK.

    The launcher used to stop at the first attribute mask that returned anything
    ("one pass is usually enough"): the masks are NOT four views of one list -- the
    long names and the hidden entries come back under the others -- and the report
    was exact: "the file list only shows 13 entries" with the count in the header
    agreeing, because both were short.  primeLua's loop is the reference (it runs
    all four and merges), and this check is about the SHAPE of the loop: a `break`
    at the top level of it is the bug coming back.
    """
    src = stripped_source()
    tree = ast.parse(src)
    fn = [n for n in ast.walk(tree)
          if isinstance(n, ast.FunctionDef) and n.name == 'list_dir_fw']
    check('finder: the launcher has the firmware listing', len(fn) == 1,
          '%d' % len(fn))
    loops = [n for n in ast.walk(fn[0]) if isinstance(n, ast.For)] if fn else []
    masks = []
    for loop in loops:
        for node in ast.walk(loop.iter):
            if isinstance(node, ast.Constant):
                masks.append(node.value)
    check('finder: ... and it tries every attribute mask',
          len(masks) >= 4, repr(masks))
    # THE SHAPE OF THE BUG: the loop stopped as soon as it had anything, i.e.
    # `if out: break` at the top level of the mask loop.  A `break` on an
    # allocation failure (the mask after this one cannot do better with no
    # memory) is legitimate, so the test looks for the accumulator, not for the
    # word break.
    bad = []
    for loop in loops:
        for stmt in ast.walk(loop):
            if not isinstance(stmt, ast.If):
                continue
            names = [n.id for n in ast.walk(stmt.test) if isinstance(n, ast.Name)]
            if 'out' not in names:
                continue
            for inner in ast.walk(stmt):
                if isinstance(inner, ast.Break):
                    bad.append(stmt.lineno)
                    break
    check('finder: ... without stopping at the first one that answers',
          not bad, 'an "if out: break" at line(s) %r' % (bad,))


def moreprint_check(emu):
    """THE CONSOLE SPEAKS moreprint WHEN THE USER HAS IT.

    The four helpers are primeLua's and primetcc's, with the same meanings:
    p_log what the launcher is doing, p_warning a fallback worth a look, p_error a
    failure, p_pass a step that worked.  A calculator without moreprint.py has to
    keep working exactly as before, so the import falls back to print().
    """
    hp = FakeHpprime()
    src = stripped_source()
    tree = ast.parse(src)
    taken = sorted(a.name for n in ast.walk(tree)
                   if isinstance(n, ast.ImportFrom) and n.module == 'moreprint'
                   for a in n.names)
    check('moreprint: the launcher takes the console helpers',
          taken == ['p_error', 'p_log', 'p_pass', 'p_warning'], repr(taken))
    defined = [n.name for n in ast.walk(tree) if isinstance(n, ast.FunctionDef)]
    check('moreprint: ... with a print fallback for a calculator without it',
          all(t in defined for t in taken), repr([t for t in taken
                                                 if t not in defined]))

    # the levels, on a launcher loaded with a recording moreprint
    mp = FakeMoreprint()
    mod, _dbg = load_main(emu, hp, moreprint=mp.module())
    mod.set_verbose(True)
    mod.progress(3, 6, 'listing the workspaces')
    mod.alert('[load] FAIL: nothing loaded')
    mod.note('[list] the firmware finder returned nothing')
    mod.succeed('[exit] status 0: the editor returned to the launcher')
    mod.log('a plain verbose line')
    check('moreprint: progress goes through p_log',
          mp.level('p_log')[:1] == ['nbCode 3/6: listing the workspaces'],
          repr(mp.level('p_log')[:2]))
    check('moreprint: a plain log line is p_log too',
          'a plain verbose line' in mp.level('p_log'),
          repr(mp.level('p_log')))
    check('moreprint: a failure goes through p_error',
          mp.level('p_error') == ['[load] FAIL: nothing loaded'],
          repr(mp.level('p_error')))
    check('moreprint: a fallback goes through p_warning',
          mp.level('p_warning') ==
          ['[list] the firmware finder returned nothing'],
          repr(mp.level('p_warning')))
    check('moreprint: a step that worked goes through p_pass',
          mp.level('p_pass') ==
          ['[exit] status 0: the editor returned to the launcher'],
          repr(mp.level('p_pass')))
    check('moreprint: nothing bypasses it (no bare print of a log line)',
          len(mp.lines) == len(mp.level('p_log')) + 3, repr(mp.lines))
    check('moreprint: ... and print() is not what it uses',
          'moreprint' in src and 'def p_log(text):' in src, '')

    # WITHOUT THE MODULE: the same call prints, which is what the launcher did
    # before the module existed
    plain, _dbg = load_main(emu, hp)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        plain.p_log('the fallback line')
    check('moreprint: without the module the line still reaches the console',
          'the fallback line' in buf.getvalue(), repr(buf.getvalue()[:60]))


def progress_check(app, mod):
    """START-UP PROGRESS: the launcher says where it is, on the console.

    A load is a second or two of a blank screen, and the launcher is the only side
    that knows what is happening.  The progress lines are printed even though the
    console is quiet by default (see log()), because they are not a log -- they are
    the thing the user is waiting for.  They also go to the durable log, so a
    start-up that dies halfway leaves the last step it reached.
    """
    import re
    src = stripped_source()
    tree = ast.parse(src)
    calls = [n for n in ast.walk(tree)
             if isinstance(n, ast.Call) and isinstance(n.func, ast.Name)
             and n.func.id == 'progress']
    check('progress: the launcher reports the load steps', len(calls) >= 5,
          '%d call(s)' % len(calls))
    steps = [n.args[0].value for n in calls
             if n.args and isinstance(n.args[0], ast.Constant)]
    check('progress: ... numbered from 1', steps[:1] == [1] and len(set(steps)) == len(steps),
          repr(steps))

    # and the lines really come out, in order, on a run
    buf = io.StringIO()
    settings, _ = mod.read_settings(app)
    with cd(app), contextlib.redirect_stdout(buf):
        mod.run_file(DEVICE_DIR + '\\hello.c',
                     {'appdir': DEVICE_DIR, 'settings': settings,
                      'workspaces': [('w', DEVICE_DIR)], 'cwd': DEVICE_DIR},
                     frames=6, max_insn=20 * 1000 * 1000)
    text = buf.getvalue()
    lines = [ln for ln in text.split('\n') if ln.startswith('nbCode ')]
    check('progress: ... on the console, in order, with a total',
          len(lines) >= 5 and lines[0].startswith('nbCode 1/') and
          '/6: ' in lines[0], repr(lines[:3]))
    got = [re.match(r'nbCode (\d+)/(\d+): (.*)', ln) for ln in lines]
    check('progress: every line names a step',
          all(g is not None and g.group(3) for g in got), repr(lines[:3]))
    logpath = os.path.join(app, 'nbcode.log')
    if os.path.exists(logpath):
        with open(logpath) as f:
            log = f.read()
        check('progress: ... and they are in the durable log too',
              '[progress] reading setting.json' in log, '')


def codec_discipline_check():
    """NO NON-STRICT ERROR HANDLER IN THE FILE THAT SHIPS.

    MicroPython does not implement them (see DeviceBytes): passing one is a
    UnicodeError, so a single `errors='replace'` anywhere in the launcher is a
    crash waiting for the wrong byte -- and the wrong byte always comes, because
    the launcher reads firmware memory and a log ring whose read windows can
    start in the middle of a character.  The runtime model in this suite catches
    it where it is used; this catches it in the source, including on paths no
    check here drives.
    """
    src = stripped_source()
    bad = []
    for node in ast.walk(ast.parse(src)):
        if not isinstance(node, ast.Call):
            continue
        fn = node.func
        if not isinstance(fn, ast.Attribute) or fn.attr not in ('decode',
                                                                'encode'):
            continue
        arg = node.args[1] if len(node.args) > 1 else None
        for kw in node.keywords:
            if kw.arg == 'errors':
                arg = kw.value
        if arg is None:
            continue
        if isinstance(arg, ast.Constant) and arg.value == 'strict':
            continue
        bad.append((node.lineno, fn.attr, getattr(arg, 'value', '?')))
    check('codec: the deployed launcher passes no non-strict error handler',
          not bad, '%d found: %r' % (len(bad), bad[:3]))
    # ... and the model that enforces it really is the device's rule, so a
    # future edit to DeviceBytes cannot quietly turn this suite back into
    # CPython-with-extra-steps
    try:
        DeviceBytes(b'\xd6\xd0').decode('utf-8', 'replace')
        model = False
    except UnicodeError:
        model = True
    check('codec: the model refuses a non-strict handler (as the device does)',
          model, '')
    check('codec: ... and still decodes valid UTF-8',
          DeviceBytes('中文'.encode('utf-8')).decode('utf-8') == '中文', '')


def ring_codec_check(app, emu, mod, dbg):
    """THE EXIT PATH'S CODEC: the ring window that starts inside a character.

    `parse_rt_ret` reads the LAST 64 BYTES of the image's log ring to find the
    status line, and nbCode's own messages are Chinese -- three bytes each -- so a
    window that opens in the middle of one is the normal case, not the corner
    case.  That is what the user saw: the editor exited and the launcher died
    with UnicodeError before it could report anything.

    The ring is built here in the emulator's memory exactly as the image writes it
    (magic, count, data), with the cut deliberately mid-character, and both
    readers -- the status parser and the log drainer -- are run against it.
    """
    # THREE CHINESE CHARACTERS ARE NINE BYTES, and the window is the last 64, so
    # the run has to sit close enough to the end for the cut to land inside it
    run = ('中文消息已保存' * 4).encode('utf-8')
    # the trailing space in the status line is load bearing: a run of 3-byte
    # characters always puts the window's first byte on a LEAD byte (which
    # decodes fine), so one extra byte is what makes the cut land on a
    # CONTINUATION byte -- the case that actually raises
    data = b'-' * 64 + run + b'\nRT_RET:7 \n'
    cut = len(data) - 64
    check('exit: a 64-byte ring window lands inside a Chinese character',
          cut >= 64 and data[cut] & 0xC0 == 0x80,
          'cut=%d byte=0x%02X' % (cut, data[cut]))

    ring = BASE + 0x100000			# scratch, well below the heap
    emu.mem[ring - BASE:ring - BASE + 8] = mod.RING_MAGIC
    struct.pack_into('<I', emu.mem, ring + mod.RING_COUNT_OFF - BASE, len(data))
    at = ring + mod.RING_DATA_OFF
    emu.mem[at - BASE:at - BASE + len(data)] = data

    code = None
    try:
        code = mod.parse_rt_ret(dbg, ring, len(data))
    except Exception as e:				# the bug being pinned
        print('    [test] parse_rt_ret raised: %r' % (e,))
    check('exit: the status is parsed out of a cut window', code == 7,
          'code=%r' % (code,))

    head = None
    with cd(app):
        try:
            head = mod.drain_ring(dbg, ring, 0)
        except Exception as e:
            print('    [test] drain_ring raised: %r' % (e,))
    check('exit: ... and the log drainer survives the same window',
          head == len(data), 'head=%r' % (head,))

    # the codec helpers themselves: the two encodings the launcher accepts, one
    # that is neither, and an encoder that does not need the device's codec
    check('exit: decode_bytes takes UTF-8, latin-1 and rubbish alike',
          mod.decode_bytes('中文'.encode('utf-8')) == '中文'
          and mod.decode_bytes(b'\xd6\xd0\xce\xc4') == 'ÖÐÎÄ'
          and mod.decode_bytes(b'\xff\xfe\x00') == '\xff\xfe\x00', '')
    check('exit: utf8_bytes is UTF-8 without the codec (the device has no '
          "'replace')",
          mod.utf8_bytes('中文 a') == '中文 a'.encode('utf-8')
          and mod.utf8_bytes('\ud800') == b'?', repr(mod.utf8_bytes('中文 a')))


def loader_paths_check(app, mod):
    """The loader's two segment-copy paths, on a TINY synthetic image.

    nbCode.elf is 482 KB, and loading it twice in this emulator costs more than
    the rest of the suite put together; a 100 byte image exercises the same code
    (segment copy, .bss clear, and the read-back that verifies it) in a fraction
    of a second.

    It is the FALLBACK that is checked here -- `copy_segments_py` and
    `clear_bss`, the path the device uses when the firmware's fread cannot be
    used -- because the fast path is what actually runs in the real load below.
    """
    payload = bytes(range(48))
    ehdr = bytearray(52 + 32)
    struct.pack_into('<4sBBBB', ehdr, 0, b'\x7fELF', 1, 1, 1, 0)
    struct.pack_into('<H', ehdr, 16, 3)              # ET_DYN
    struct.pack_into('<H', ehdr, 18, 40)             # EM_ARM
    struct.pack_into('<I', ehdr, 24, 0x1000)         # e_entry
    struct.pack_into('<I', ehdr, 28, 52)             # e_phoff
    struct.pack_into('<H', ehdr, 42, 32)             # e_phentsize
    struct.pack_into('<H', ehdr, 44, 1)              # e_phnum
    # PT_LOAD: file offset 84, vaddr 0x1000, 48 B of file, 96 B of memory
    struct.pack_into('<8I', ehdr, 52, 1, 84, 0x1000, 0x1000, 48, 96, 5, 4)
    data = bytes(ehdr) + payload

    segs = mod.ElfTools.segments(data)
    check('loader: the synthetic image parses as one PT_LOAD segment',
          segs == [(84, 0x1000, 48, 96)], repr(segs))

    loader = mod.ShellcodeElfLoader(_DEBUG)
    loader.image_raw = _DEBUG.call(mod.ADDR_MALLOC, 512)
    loader.image_base = loader.image_raw
    loader.owned = True
    try:
        loader.copy_segments_py(data)
        loader.clear_bss(data)
        got = bytes(_DEBUG.read_mem(loader.image_base + 0x1000, 96))
        check('loader: the fallback copy puts the bytes where the ELF says',
              got[:48] == payload, repr(got[:8]))
        check('loader: ... and zeroes the .bss behind them',
              got[48:] == b'\x00' * 48, repr(got[48:56]))
    finally:
        _DEBUG.call(mod.ADDR_FREE, loader.image_raw)

    # THE CHUNKED READ PATH, against the emulator's own file API.  The firmware's
    # fread is used three ways (whole segment, 64 KB, 8 KB) because nothing states
    # whether it will move 471 KB in one call; this checks the smallest of them,
    # which is the last resort before the word at a time copy.
    tiny = os.path.join(app, 'tiny.elf')
    with open(tiny, 'wb') as f:
        f.write(data)
    mem = mod.MemUtils(_DEBUG)
    pname = mem.alloc_wstr('tiny.elf')
    pmode = mem.alloc_wstr('rb')
    fd = 0
    try:
        fd = _DEBUG.call(mod.FW_FOPEN, pname, pmode)
    finally:
        mem.free(pname)
        mem.free(pmode)
    check('loader: the firmware opens a file the test wrote', bool(fd),
          'fd=%r' % (fd,))
    if fd:
        dest = _DEBUG.call(mod.ADDR_MALLOC, 128)
        try:
            got = loader.read_chunked(fd, 84, dest, 48, 16, data)
            check('loader: the chunked firmware read works (16 byte chunks)',
                  got is True, 'read_chunked -> %r' % (got,))
            check('loader: ... and it landed the right bytes',
                  bytes(_DEBUG.read_mem(dest, 48)) == payload, '')
        finally:
            _DEBUG.call(mod.ADDR_FREE, dest)
        _DEBUG.call(mod.FW_FCLOSE, fd)


def reuse_check(app, mod, dbg):
    """A SECOND RUN MUST RE-ENTER THE IMAGE, NOT RELOAD IT.

    Loading is the expensive phase of a start-up (the ELF is copied into the
    firmware heap and its relocations applied -- a few hundred debug round trips,
    or a hundred thousand if the firmware will not do the copying).  None of it
    changes between runs of the same file, so the second run reuses the block; this
    checks that it really does, and that the reused image is still a *working*
    editor (the settings are re-read on every entry, so reuse must not mean "the
    same session again").
    """
    logpath = os.path.join(app, 'nbcode.log')
    # the image rewrites its own log file and keeps only the last 2 KB of it, so
    # this run gets a clean one: what we are looking for is written at the end, and
    # nothing else should be in the way
    if os.path.exists(logpath):
        os.remove(logpath)
    cost_before = getattr(dbg, 'round_trips', 0)
    settings, _ = mod.read_settings(app)
    with cd(app):
        status = mod.run_file(DEVICE_DIR + '\\hello.c',
                              {'appdir': DEVICE_DIR, 'settings': settings,
                               'workspaces': [('pCode.hpappdir', DEVICE_DIR)],
                               'cwd': DEVICE_DIR},
                              frames=30, max_insn=6 * 1000 * 1000)
    spent = getattr(dbg, 'round_trips', 0) - cost_before
    text = ''
    if os.path.exists(logpath):
        with open(logpath) as f:
            text = f.read()
    check('reuse: the second run exits cleanly', status == 0,
          'status %r' % (status,))
    check('reuse: it says it re-entered the image already in memory',
          'reusing the image' in text, repr(text[-300:]))
    load_line = [ln for ln in text.split('\n')
                 if 'image load / reuse' in ln]
    check('reuse: the load phase cost almost nothing the second time',
          bool(load_line) and int(load_line[-1].split()[-3]) < 30,
          repr(load_line[-1:]))
    check('reuse: ... and the whole start-up is far cheaper than a load',
          spent < 120, '%d round trips' % spent)
    if VERBOSE:
        for ln in text.split('\n'):
            if '[cost]' in ln or 'reusing' in ln:
                print('    | ' + ln)


def finder_merge_check(mod):
    """One row per file, however many times the finder reports it.

    The firmware finder is asked once per attribute mask (that is how long names
    and hidden files come back), and every pass lists the same files again.  The
    start screen showed every name FOUR TIMES until this was merged, so the merge
    is checked against exactly that shape.
    """
    passes = [('hello.lua', 180, False), ('notes.txt', 190, False)]
    rows = passes * 4
    check('files: four finder passes give one row per file',
          mod.merge_find_rows(rows) == [('hello.lua', 180, False),
                                        ('notes.txt', 190, False)],
          repr(mod.merge_find_rows(rows)))
    # a pass that knows a size where an earlier one did not fills it in, and the
    # first spelling of a name wins
    got = mod.merge_find_rows([('x.c', 0, False), ('X.C', 331, False),
                               ('y.c', 12, False)])
    check('files: a later pass fills in a missing size',
          got == [('x.c', 331, False), ('y.c', 12, False)], repr(got))
    check('files: nothing is lost when the names are all different',
          len(mod.merge_find_rows([('a', 1, False), ('b', 2, False),
                                   ('c', 3, False)])) == 3, '')


def input_ownership_check(mod):
    """THE IMAGE MAY NOT BE FREED WHILE THE OS IS INSIDE ITS HOOK.

    The OS's input thread reaches the editor through the patched get_event slot
    and blocks in the firmware's get_event from inside the hook, so when the
    editor exits that thread is usually still inside the image.  Freeing it then
    resumes the thread in freed memory on the next event -- which rebooted the
    calculator when the user pressed esc twice.  The launcher's rule is therefore
    "free when hook_busy says zero, otherwise keep the block for the next load",
    and this checks both halves against the emulator's own heap accounting.
    """
    fw = _DEBUG.emu.fw
    mod._held = []              # this test allocates its own blocks

    def live(addr):
        return addr in fw.heap

    # THE DEFAULT IS "KEEP IT MAPPED": a finished session leaves the image
    # resident so the next run re-enters it instead of loading it again (loading is
    # the expensive phase, and 604 KB on a 32 MB machine is cheap next to a
    # start-up).  `unload(keep=False)` is the explicit "give it back", and it
    # refuses while the OS could still be inside the image.
    fw = _DEBUG.emu.fw
    mod._held = []

    def live(addr):
        return addr in fw.heap

    def make(busy):
        loader = mod.ShellcodeElfLoader(_DEBUG)
        raw = _DEBUG.call(mod.ADDR_CALLOC, 64, 1)
        loader.image_raw = raw
        loader.image_base = raw
        loader.image_size = 64
        loader.owned = True
        loader.state_addr = raw + 16
        _DEBUG.write_mem(loader.state_addr + mod.HOOK_BUSY_OFF, busy)
        return loader, raw

    # (1) a hook call in flight: kept whatever the caller asks for
    loader, raw = make(1)
    loader.unload(keep=False)
    check('input: a block with a hook call inside it is NOT freed',
          live(raw) and len(mod._held) == 1,
          'live=%r held=%d' % (live(raw), len(mod._held)))

    # (2) the hook has returned: release_held() gives it back
    _DEBUG.write_mem(loader.state_addr + mod.HOOK_BUSY_OFF, 0)
    loader.release_held()
    check('input: ... and it is given back once the hook has returned',
          not live(raw) and not mod._held,
          'live=%r held=%d' % (live(raw), len(mod._held)))

    # (3) the ordinary end of a session: the image STAYS mapped for reuse
    loader, raw = make(0)
    loader.unload()
    check('input: a finished session keeps the image mapped (the next run '
          'reuses it)', live(raw) and loader.owned and len(mod._held) == 1,
          'live=%r held=%d owned=%r' % (live(raw), len(mod._held), loader.owned))

    # (4) ... and an explicit release does give it back
    loader.unload(keep=False)
    check('input: unload(keep=False) gives it back at once when it is safe',
          not live(raw) and not mod._held, 'live=%r held=%d'
          % (live(raw), len(mod._held)))

    # (5) THE SLOT STILL POINTS INTO THE IMAGE: not freeable either, no matter how
    # idle the hook looks -- the OS can enter the block on the next event
    loader, raw = make(0)
    save_slot = bytes(_DEBUG.read_mem(mod.FW_EVENT_SLOT, 4))
    _DEBUG.write_mem(mod.FW_EVENT_SLOT, mod.SLOT_TRAP)
    loader.unload(keep=False)
    check('input: a block the slot can still enter is NOT freed',
          live(raw) and len(mod._held) == 1,
          'live=%r held=%d' % (live(raw), len(mod._held)))
    _DEBUG.write_mem(mod.FW_EVENT_SLOT, struct.unpack('<I', save_slot)[0])
    loader.unload(keep=False)
    check('input: ... and once the slot is clear it can go',
          not live(raw) and not mod._held, 'live=%r' % (live(raw),))

    # (6) an image that never reached the hook (a failed load) is freed too
    loader, raw = make(0)
    loader.state_addr = 0
    loader.unload(keep=False)
    check('input: a block with no state block is freed, not held',
          not live(raw) and not mod._held, 'live=%r' % (live(raw),))


def console_quiet_check(app, mod):
    """THE CONSOLE IS QUIET, AND THE LOG FILE IS NOT.

    Every [load]/[cost]/[hook] line used to be PRINTED at start-up and the cost
    table again on the way out, which is noise in the user's face on every run --
    and since the editor runs inside ONE debug call, start-up and exit are all the
    output there ever is.  The durable file keeps every line (that is what a crash
    is diagnosed from, and the rest of this suite reads it), an empty `verbose`
    marker brings the printing back, and a genuine failure still speaks.
    """
    marker = os.path.join(app, mod.VERBOSE_MARKER)
    mod._verbose = None				# drop the cached answer
    check('console: a normal session does not print', mod.verbose() is False, '')
    with open(marker, 'w') as f:
        f.write('')
    mod._verbose = None
    check('console: ... and the marker file turns the printing back on',
          mod.verbose() is True, '')
    os.remove(marker)
    mod._verbose = None
    check('console: ... and removing it is quiet again',
          mod.verbose() is False, '')

    # log() writes to the file and says nothing; alert() does both
    with cd(app):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            mod.log('quiet line for the file only')
        check('console: log() keeps the console clean', buf.getvalue() == '',
              repr(buf.getvalue()))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            mod.alert('a failure the user has to see')
        check('console: alert() still speaks', 'has to see' in buf.getvalue(),
              repr(buf.getvalue()))
    with open(os.path.join(app, 'nbcode.log')) as f:
        written = f.read()
    check('console: ... and both lines are in the durable log',
          'quiet line for the file only' in written
          and 'a failure the user has to see' in written, '')


def clipboard_check(app, emu, mod):
    """THE GLOBAL CLIPBOARD, ON THE REAL IMAGE.

    shift+VIEW is the copy chord (shift+VIEW in the editor), and the point of it
    is that the entry OUTLIVES the session: nb_clip.c writes the list to a file
    next to nbcode.log.  This drives the real image with the chord and reads the
    file from the outside, which is the only way to check the two things the host
    tests cannot: that the device path is built from the launch config, and that
    the file really lands in the application folder.
    """
    path = os.path.join(app, 'nbclip.txt')
    if os.path.exists(path):
        os.remove(path)
    logpath = os.path.join(app, 'nbcode.log')
    log_before = os.path.getsize(logpath) if os.path.exists(logpath) else 0
    settings, _ = mod.read_settings(app)
    # SHIFT LATCHES ON ITS PRESS, and VIEW copies: four events, delivered one per
    # get_event poll while the editor runs
    emu.fw.inject_key(0x8B, True)
    emu.fw.inject_key(0x8B, False)
    emu.fw.inject_key(0xB4, True)
    emu.fw.inject_key(0xB4, False)
    with cd(app):
        status = mod.run_file(DEVICE_DIR + '\\hello.c',
                              {'appdir': DEVICE_DIR, 'settings': settings,
                               'workspaces': [('w', DEVICE_DIR)],
                               'cwd': DEVICE_DIR},
                              frames=30, max_insn=6 * 1000 * 1000)
    check('clip: the session with the copy chord exits cleanly',
          status == 0, 'status %r' % (status,))
    raw = b''
    if os.path.exists(path):
        with open(path, 'rb') as f:
            raw = f.read()
    check('clip: shift+VIEW wrote the clipboard into the application folder',
          raw.startswith(CLIP_MAGIC),
          repr(raw[:40]))
    check('clip: ... holding the line the caret was on',
          b'int main(void) {' in raw, repr(raw[:80]))
    text = ''
    if os.path.exists(logpath):
        with open(logpath) as f:
            text = f.read()[log_before:]
    check('clip: ... and the image reads it back at start-up',
          'clip entries=' in text,
          repr([ln for ln in text.split('\n') if 'clip' in ln][:3]))


def probe_check(app, mod):
    """main.probe(): the recovery path the README tells a user to run first.

    It is the only launcher entry point that walks the whole load chain without
    starting the editor, so if it is broken the "what do I do when it will not
    start" story is broken -- and nothing else in this suite would notice.
    """
    buf = io.StringIO()
    with cd(app), contextlib.redirect_stdout(buf):
        rc = mod.probe(DEVICE_DIR)
    text = buf.getvalue()
    check('probe: it completes the load chain', rc == 0, 'rc=%r' % (rc,))
    check('probe: it reports the entry point it found',
          'loader -> entry 0x' in text, repr(text[-200:]))
    check('probe: it lists the application folder', '[probe] listing' in text,
          repr(text[-300:]))
    check('probe: it reports where the ring and the image ended up',
          '[probe] ring 0x' in text and 'image base 0x' in text,
          repr(text[-200:]))
    if VERBOSE:
        for line in text.split('\n'):
            if line:
                print('    | ' + line)


def main():
    if not os.path.exists(ELF):
        print('nbCode.elf is missing -- run `make` first')
        return 2
    print('nbCode emulator tests')
    image_checks('image')
    root, app, emu, mod, dbg = launcher_config_check(image_checks('image2'))
    try:
        device_constraints_check(app, mod)
        stripped_launcher_check(app, mod)
        codec_discipline_check()
        console_quiet_check(app, mod)
        ring_codec_check(app, emu, mod, dbg)
        loader_paths_check(app, mod)
        finder_merge_check(mod)
        input_ownership_check(mod)
        run_editor_check(root, app, emu, mod, dbg, read_elf())
        reuse_check(app, mod, dbg)
        probe_check(app, mod)
        # LAST: this one drives a run of its own, and the load/reuse checks above
        # count loads (an image left resident by an earlier run would make them
        # read 0)
        progress_check(app, mod)
        moreprint_check(emu)
        finder_masks_check()
        workspace_walk_check(app, mod, emu)
        # AFTER the walk check: this one drives a run of its own, and the walk
        # check counts the run's log lines
        clipboard_check(app, emu, mod)
    finally:
        if KEEP:
            # -k keeps the fake calculator folder, so the durable log the image
            # wrote (and everything else a run left behind) can be read: this is
            # the same log a device run leaves in nbCode.hpappdir\nbcode.log
            print('   [test] kept %s' % app)
        else:
            shutil.rmtree(root, ignore_errors=True)

    fails = [r for r in RESULTS if not r[1]]
    print('\n%d checks, %d failure(s)' % (len(RESULTS), len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
