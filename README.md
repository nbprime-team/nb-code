# nbCode -- a code editor for the HP Prime G1, written in C

nbCode is a native code editor for the HP Prime G1 graphing calculator.  It is
not a PPL program and not a Python program: **nbCode.elf** is a freestanding
ARMv5TE binary that the calculator's MicroPython application loads into the
firmware heap and runs, the same way DOOM, primetcc and primeLua are loaded.
The launcher (`main.py`) reads the settings, lists the folders, writes one small
launch file, starts the image, and then gets out of the way -- the editor owns
the screen and the keyboard until the user quits.

```
 hello.c *                                          C     <- name (* = modified)
 +--------------------------------------------------+     and the language
 |   1  #include <stdio.h>                           |     the highlighter chose
 |   2                                              |
 |   3  /* a 40 column, 13 line window:              |
 |   4     Unifont western + GB2312, syntax colour   |
 |   5     for C, Lua, Python, PPL and plain text    |
 |   6  */                                           |
 |   7  int main(void)                               |
 |   8  {                                            |
 |   9      printf("hello, nbCode\n");                |
 |  10      return 0;                                |
 |  11  }                                            |
 |  12                                              |
 +--------------------------------------------------+
 | 12:1 U8  331B                                    |     <- line:column, the
 +--------------------------------------------------+        encoding, the size
```

* **Written in C**, built for `arm926ej-s`, position independent, no libc: the
  string core, the heap wrapper, the font and the editor are all in the image.
* **Loaded by Python**: `nbCode.hpappdir\main.py` (MicroPython) is the launcher.
* **Syntax highlighting** for C, Lua, Python, PPL (HP Prime PPL) and plain text,
  chosen from the file name or forced in `setting.json`.
* **A real file manager start screen**: workspaces, then the files in the one you
  pick (the idiom comes from the Python PrimeCode's file manager).
* **Unifont** for the face: the western repertoire plus the GB2312 Chinese set,
  8x16 for half-width and 16x16 for full-width cells, ~8,500 glyphs.
* **Keyboard driven**, using the firmware's own `get_event` stream: no PPL
  `GETKEY` polling, no missed keys.

---

## 1. Install and run

1. Copy the whole `nbCode.hpappdir` folder into `C:\DATA\` on the calculator
   (the Connectivity Kit does this; the folder name must keep the `.hpappdir`
   suffix).
2. Run it:
   * **Applications → nbCode → main.py**, or
   * from the MicroPython app: `import main` then `main.run()`, or
   * open one file directly:
     `import main` / `main.run("C:\\DATA\\nbCode.hpappdir\\hello.c")`.

The first screen lists **workspaces** (folders).  `ENTER` opens the selected one
and shows its files; `ENTER` on a file opens it in the editor.  `ON` quits.

The release zip also carries three tiny sample files (`hello.c`, `hello.lua`,
`notes.txt`) so that the first run has something to open.  If you build from
source instead of using the release zip, see §5.

---

## 2. Keys

The keyboard has no room for a menu bar, so every action is a key.  `F6` draws
this table on the screen, which is how a user finds out what the keys do.

### Start screen (workspaces)

| key | action |
|---|---|
| `up` / `down` | move in the list |
| `ENTER` / `right` | open the selected workspace (or, in the file list, the selected file) |
| a letter | jump to the next entry starting with that letter |
| `esc` | quit nbCode (from the workspace list) / back to the workspace list (from the files) |
| `ON` | quit nbCode |

### File list

| key | action |
|---|---|
| `up` / `down` | move |
| `ENTER` / `right` | open the file |
| `F1` | back to the workspace list |
| `F2` | new file (type a name, `ENTER` creates it) |
| `F3` | delete the selected file (asks first) |
| `F4` | refresh the list |
| `esc` | back to the workspace list |

### Editor

| key | action |
|---|---|
| arrows | move the cursor (up/down keep the column) |
| `ENTER` | new line, auto-indented |
| `BKSP` | delete backwards, joining lines at the start of one |
| `Del` (the key marked with a backspace arrow) | the same in the editor; **deletes the file** in the file list |
| `alpha` | letters: press twice to lock.  The status bar says `alpha`, `alock` (lower case) or `Alock` (capitals) |
| `shift`+key | the symbol on the key (`:` `;` `,` `.` `?` `!` `"` …) |
| `symb` / `plot` / `num` / `cas` | `(` `)` `{` `}` -- the keypad has no bracket keys |
| `shift`+`ON` | a real TAB character (inserts `tab_width` spaces) |
| `MENU` | the screen's own menu: save, save as, undo, copy/cut, clipboard, file info, go to… |
| `shift`+`VIEW` | **copy**: the selection when there is one, otherwise the caret's line |
| `shift`+`MENU` | the **global clipboard**: the last ten things copied, from any file.  Tap one to insert it, `Del` to throw it away |
| `shift`+`A` | the character table |
| `shift`+`6` | the quick symbol panel (numbers in keypad order) |
| `shift`+`←` / `→` | start / end of the line |
| `shift`+`BKSP` | delete forwards |
| `esc` | file list (offers to save first if the file is modified) |
| `HELP` | the key help overlay (`esc`, `ON` or `HELP` closes it) |
| `ON` | quit (asks first if the file is modified) |
| tap a line number | fold / unfold the block on that line |

Twelve code lines of 16 pixels each fit between the header and the status bar,
with a thirteenth peeking in at the bottom, so a full-width western line is 40
characters and a Chinese line is 20.  Horizontal scrolling moves in 8-pixel steps,
so a Chinese character is never half scrolled off the left edge.  The status bar
shows `line:column`, the encoding the file was detected as (`U8`, `GB`, `L1`), a
read-only marker, the size, the **abbreviated file type** (`py`, `c`, `lua`,
`ppl`, `txt`) and the keyboard layer (`shift` / `alpha` / `alock` / `Alock`).

---

## 3. Settings

`nbCode.hpappdir\setting.json` is the user's file.  The launcher reads it, and a
syntax error in it is **not** fatal: the launcher logs the problem and uses the
defaults, because an editor that will not start is worse than an editor with the
wrong tab width.

```json
{
  "theme": 0,
  "tab_width": 4,
  "show_line_numbers": true,
  "highlight": true,
  "indent_guides": true,
  "readonly": false,
  "language": "auto",
  "workspaces": [],
  "default_workspace": 0
}
```

| key | meaning |
|---|---|
| `theme` | `0` light (default), `1` dark |
| `tab_width` | `2`–`8`, spaces per indent (`4`) |
| `show_line_numbers` | the 4-column line number gutter |
| `highlight` | syntax colouring on/off |
| `indent_guides` | the indentation hairlines in the editor |
| `complete` | the completion popup (on unless set to `false`) |
| `readonly` | refuse to write: the editor then only reads |
| `language` | `auto`, `c`, `lua`, `py`, `ppl`, `txt` -- `auto` uses the file name |
| `workspaces` | extra folders to offer on the start screen: a list of `["label", "C:\\DATA\\somewhere"]` pairs, or plain paths |
| `default_workspace` | which entry the start screen opens on |

`nbcode.cfg` and `nbcode.log` in the same folder are **not** settings: the first is
the launch file the launcher writes for the image on every run, the second is the
log the image writes (see §6).  Both are safe to delete.

---

## 4. How it works

### 4.1 The two halves

```
  calculator                                the image (nbCode.elf)
  ----------                                ----------------------
  MicroPython app
    main.py
      reads setting.json
      lists folders and files      --->     nbcode.cfg  (written next to the
      writes nbcode.cfg                        image: settings, workspaces,
                                              file list, file to open, log path)
      loads the ELF into the
        firmware heap
      calls nb_entry()
      reads the log ring  <--------------    writes log lines into a ring with
        when it returns                        the magic "NBCORING"
      parses "RT_RET:<n>" <--------------    writes the exit status there too
      restores the firmware's  <---------    patches get_event with a
        get_event stub                        trampoline into the image
```

The image is loaded by the shellcode loader DOOM, primetcc and primeLua use:
`malloc` an image block, copy the `PT_LOAD` segments through the debug channel,
apply the `R_ARM_RELATIVE` relocations, call the entry point.  Nothing is linked
against the firmware: every firmware service is reached through its SVC stub
(`port/nb_svc.s`), the same table primeLua measured.

### 4.2 The launch file, not a launch struct

The launcher and the editor have to agree on a few settings and on which file to
open.  The obvious mechanism -- a C struct in memory, handed over in `r0` -- was
tried, twice, and both versions failed the same way: the two sides disagreed
about the layout, the editor read a pointer where a count belonged, found a zero
magic, and quit with a black screen.  Writing the struct into the image's `.bss`
tail was worse: that region is cleared by the loader and reused by the heap, so
the config read back as zeros.

So the handoff is a **file** (`nbcode.cfg`, format in `src/nb_cfg.h`): a header of
little-endian words and then NUL-terminated UTF-8 strings.  A file has no layout
to disagree about, a short file or a wrong magic is *detected*, and the failure
mode is "start with defaults and say so in the log", never a black screen.
`tools/make_dist.py` reads the magic, the version and the file name out of the C
header and refuses to package the release if `calc/main.py`'s copy of them has
drifted.

### 4.3 The keyboard

primeLua's mechanism, which comes from PureDOOM: the firmware's `get_event` slot
is patched with a two-instruction trampoline (`ldr pc, [pc, #-4]` + the hook
address) using the CP15 cache ritual, so every key event the OS decodes is also
handed to the image, on the OS's own input thread.  The hook decodes the device
key code, queues an item, and the editor drains the queue once per frame.  The
launcher restores the firmware's original bytes when the run ends, so a crashed
nbCode cannot leave a dead keyboard behind.

### 4.4 The log

The image cannot print: there is no console in a loaded binary.  It writes into
a ring inside its own data (`"NBCORING"`), and the launcher reads it out when the
session ends -- so a crash still tells you what nbCode was doing.  The run itself
is **one** debug call that returns when the editor exits (deliberately not
primeLua's thread mode, which spins polling the ring the whole time: nbCode's
editor owns the screen, and the messages that matter are the ones it writes to
`nbcode.log` through the firmware's file API, which survives a reset that the ring
does not).  The exit status travels the same way as `RT_RET:<n>`, because the
debug interface's return value is not trustworthy on this hardware.

A log line is `tag=value` with the value in decimal (`cfg: workspaces=1`,
`frame count=48`), except for bit patterns where hex is the readable form
(`cfg flags=00000000`, a magic number).

### 4.5 Drawing

One ARGB8888 framebuffer, 320x240, drawn by `nb_gfx.c` and pushed with a single
blit per frame.  Drawing straight to the visible buffer flickers; that is why
there is a back buffer, and why the frame loop is a fixed rate.  Text is 8x16 per
western glyph and 16x16 per Chinese glyph, from the Unifont face compiled into
the image.

---

## 5. Building

Requirements: `arm-none-eabi-gcc` (Ubuntu package `gcc-arm-none-eabi`), `make`,
`python3`, and GNU Unifont at `/usr/share/unifont/unifont.hex` **only** if you
regenerate the font (the generated face is committed, so a plain build does not
need it).

```sh
make                # build nbCode.elf and deploy it to ../nbCode.hpappdir/
make deploy         # re-deploy without rebuilding
make dist           # validate, then write dist/nbCode.hpappdir.zip + nbCode-src.zip
make font           # regenerate port/nbcode_font_data.c from Unifont, then rebuild
make test           # host tests + emulator tests
make test-host      # the editor's logic on the PC     (571 checks)
make test-emu       # the real image on an ARM emulator (132 checks)
make clean
```

`make deploy` runs `tools/strip_prose.py` over the launcher on the way out: the
calculator compiles a module's whole source into its own memory, and 42% of
`calc/main.py` is comments and docstrings.  The repository keeps every word; the
copy that ships is the stripped one, and `tools/make_dist.py` refuses to package a
`main.py` that still has them.

`make dist` is not just a zip: it parses the ELF and refuses to package an image
that the calculator could not run -- a relocation the loader cannot apply (it
applies `R_ARM_RELATIVE` and nothing else), no `PT_LOAD` segment, an entry point
outside every segment, a missing `NBCORING` ring, or a launcher whose config magic
or version does not match `src/nb_cfg.h`.

The build is self-contained: nothing from primetcc or primeLua is compiled in.
`tests/emu_arm2.py` is the one vendored file (the ARM interpreter used by
`make test-emu`), and a sibling `primetcc` checkout takes precedence over it.

### Sizes (v1.0, `-O2`)

| | |
|---|---|
| deployed `nbCode.elf` | 487 KB (debug info stripped) |
| bytes the loader copies | 476 KB of `PT_LOAD` segments |
| firmware heap the loader asks for | 624 KB, held for the session and reused between runs |
| relocations | 63, all `R_ARM_RELATIVE` |
| firmware round trips for a start-up | 197 the first time (the image is loaded BY THE FIRMWARE), 26 afterwards (it stays mapped) |
| font bitmaps | 8,518 glyphs, 124,326 words (~486 KB) |

---

## 6. When it does not start

The image is loaded into the firmware heap and run there, so a failure looks the
same from the outside: a black screen or a reboot.  In order:

1. **`main.probe()`** from the MicroPython app: it walks the load chain (a bare
   name reads, the settings, the workspace list, the folder listing, the ELF
   header, the image allocation, the relocations, the ring magic) and logs each
   step *before* it happens, so the last line names the step that failed.
2. **`nbcode.log`** in the application folder: written by the image itself, so it
   survives a reset.  `cfg: no nbcode.cfg (…): defaults` means the launcher could
   not write the launch file; `cfg: bad magic …` means the two sides are from
   different builds; `frame count=…` every 16 frames is the editor's heartbeat.
3. **The keyboard**: if keys do nothing after a crash, the input slot was not
   restored -- `main.stand_down_input()` puts the firmware's own bytes back.
4. **Memory**: nbCode asks the loader for 649 KB on a 32 MB machine, and the
   editor's own buffers (the file, the per-line style cache) come from the same
   heap.  Opening a 1 MB source file is refused rather than attempted
   (`NB_FILE_MAX`).

Known limits, stated rather than discovered: files up to 1 MB, 256 entries per
listing, 8 workspaces, 128 undo steps, and no line wrapping (a long line scrolls
horizontally).

### 6.1 What the calculator does not have

Each of these was a real failure on hardware before it was a rule in the code,
and each one is now a test in `tests/test_emu.py`:

| the calculator has no | so the launcher |
|---|---|
| `struct` (it is called `ustruct`) | imports `ustruct as struct`, with CPython's `struct` only as the host fallback |
| `time` | imports none: nothing in the launcher sleeps or measures time |
| `json` / `ujson` | parses `setting.json` with its own parser (`parse_json`), checked against CPython's `json` in the tests |
| `os` for anything useful | lists folders through the **firmware's** finder (`W_FIND_FIRST`/`W_FIND_NEXT`), stats with `W_GET_ATTR`, and asks for the working directory with `W_GETCWD` |
| `__name__ == '__main__'` | keys its entry point off the interpreter (`sys.implementation.name`) instead |
| absolute paths in its file API | opens files by **bare name** only: `"C:\DATA\nbCode.elf"` is `OSError 0` there |

The last row is the interesting one, because the two halves of nbCode need
opposite spellings of the same file:

```
    launcher (MicroPython)          image (firmware file API)
    "nbcode.cfg"  ---- bare name     "C:\DATA\nbCode.hpappdir\nbcode.cfg"
       |                                        |
       +------------ one file ------------------+
```

So the launcher writes the launch file by bare name, and hands the image the
application folder **in r0** at entry (a pointer to a NUL-terminated UTF-8 path,
not a struct -- a string has no layout for the two sides to disagree about).  The
image copies it, uses it as the device spelling, and falls back to its build-time
constant and then to the bare name if it has to.

### 6.2 The load is 197 firmware round trips, not 124,000

A debug call is a device write plus a read.  Writing a 482 KB image through that
protocol one **word** at a time is ~124,000 of them -- minutes on the calculator,
and the reason the first hardware run looked hung.  Two firmware services avoid
it:

* **`calloc` (svc 0x10038)** for the image block instead of `malloc`: the block
  arrives zeroed, so the image's 116 KB of `.bss` never has to be written at all;
* **`fread` (svc 0x100D4)** with the destination pointer: one call copies a whole
  `PT_LOAD` segment straight into that block, instead of one call per word.

`tests/test_emu.py` now budgets the whole session at **under 8,000 round trips**
(it measures 197 to load and 222 for the whole start-up; §6.9 replaced this with
the in-firmware loader) and checks that both fast paths were the ones used, so a change
that quietly goes back to the slow path fails the suite.  The slow path is still
there and still tested (on a tiny synthetic image): it is what runs if the
firmware refuses either service.

### 6.3 The LCD is behind a pointer, and the emulator let that slide

On the first hardware run the editor wrote its own log line saying it had
started, then the calculator faulted and rebooted.  The cause was one line:

```c
    p = (unsigned *)get_lcd();
    if (p[0] >= 0x30000000u && p[0] < 0x32000000u)
            fb = (nb_u32 *)p[0];        /* WRONG: p[0] is the INFO struct */
```

The firmware hands out an LCD *structure*; its first field is the address of the
LCD **info** structure, and the pixels are at `*(u32 *)(info + 16)`, with the size
at `info + 2` / `info + 4`.  nbCode took `p[0]` -- the info struct's address -- as
the framebuffer, and because nbCode draws straight into the buffer it is handed,
every frame was written into firmware memory.  The range check above cannot catch
that: the wrong pointer is perfectly in range.

The emulator could not catch it either, and that is the interesting part: there
the info struct sits 64 bytes **before** the real framebuffer, so the same bug
produced a picture that was merely shifted by 16 pixels and a slightly scribbled
struct -- the editor ran, and every check passed.  On the device it is a reset.

`tests/test_emu.py` now asserts the two things that were missing: the firmware's
framebuffer must have been drawn into, and the LCD info structure must still read
`w=320 h=240` with its own pointer.  Re-introducing the old line fails the suite
with `w=65310 h=14986` -- pixel data where the firmware's structure should be.

`port/nb_screen.c` also refuses to guess: if any step of that chain is missing or
the address is implausible, it returns 0, `nb_run_begin()` stops before drawing
anything, and the reason goes to the log.  An editor that does not start is a bug
report; an editor that writes 307 KB into the firmware is a reset with no trail.

### 6.4 Two more things the hardware taught, now with tests

**The glyph origin was a whole cell to the left.**  Unifont's cells are 8 px
(west) and 16 px (full width), and the generated table stores each glyph's ink
box *inside its cell* (`glyph_x0` 0..7, `glyph_xw` the inked width, with the ink
in the LOW bits of the row word: `'A'` row 0 is `0x000C`).  nbCode drew half-width
glyphs at `x0 - 8`, believing their ink lived in the high byte -- so every
western character sat eight pixels left of its cell, with the cursor one cell to
the right of it.  The host tests could not see it: they drew at x=0, where a
negative origin is clipped away, and the glyph-cache comparison compared two
paths that shared the same wrong rule.  `tests/test_host.c` now measures where
the ink actually lands.

**The image may not be freed while the OS is inside its hook.**  The OS's input
thread reaches the editor through the patched `get_event` slot, and the call the
hook makes (the firmware's own `get_event`) **blocks** until an event arrives --
so for most of a session that thread is inside the image.  Quitting the editor
left it there, the launcher freed the block, and the *next* event resumed the
thread in freed memory: on the calculator, "press esc at the workspace list and
it reboots".  The input state now carries a `hook_busy` counter, and
`ShellcodeElfLoader.unload()` frees the block only when it reads zero; otherwise
the block is held and released before the next load, by which time the hook has
returned.  Both halves are tested against the emulator's own heap accounting.

### 6.5 Green files, or: the state array that was never initialised

"Some files open with syntax colouring and some open all green" was one bug, and
green is `NB_HS_STRING`.  The highlighter keeps **one byte per line** -- the block
state that line starts in -- in an array allocated by `nb_hl_lines()`:

```c
    if (!h->state_line || h->state_cap < buf->nlines + 2) {
            unsigned char *n = nb_realloc(h->state_line, cap);
            ...
            h->state_line = n;              /* the fresh bytes: NOT zeroed */
    }
    state = h->state_line[first];           /* read as the block state */
```

`realloc` does not zero, and that byte is read by the next line's tokenizer as
"are we inside a block comment or a long string?".  Heap garbage in
`{NB_HL_BLK_LUA_LONG, NB_HL_BLK_PY_TRIPLE_D, NB_HL_BLK_PY_TRIPLE_S}` paints the
WHOLE FILE in the string colour.  Which files were affected depended on what the
heap happened to hold, which is why it looked random -- and why the same bug made
the four duplicate rows of one file (the old list bug) render differently.

`nb_hl_init()` made it worse: it zeroed the whole highlighter struct, *including
the state array pointer*, so every opened file leaked the previous array and then
allocated a fresh, uninitialised one.  The fix is a single entry point for opening
a file, `nb_hl_open()`, which keeps the array (tagged with a magic so it is safe
on a struct that has never been initialised) and `nb_hl_lines()` zeroes whatever
it grows.

The tests poison the heap with a "Python triple quote" state before highlighting a
plain line, so the failure is deterministic instead of one in four: without the
fix, `int x = 1;` reports its `x` as `NB_HS_STRING` -- a green file, reproduced in
a host test.

### 6.6 Leaving the editor, and what an edit invalidates

**ESC asks where you are going.**  ESC and F1 mean "back to the file list"; ON means
"out of the program".  Both ask about unsaved changes -- with the *same* kind of
question, and that was the bug: one question, "save before quit?", was used for
both, so pressing ESC and answering 'n' quit the whole editor instead of closing
the file.  The question now carries its destination (`prompt_then`), the answers are
**three** ways (y = save, n = leave without saving, ESC = cancel and stay in the
file), and the tests drive all of it: ESC+n goes to the browser, ON+n quits, ESC on
the question leaves the file open.

**An edit invalidates more than the line you typed on.**

* a line's style buffer is `len+1` bytes, and `nb_hl_line()` writes **one style byte
  per byte of the line**.  It used to be allocated only when missing, so typing one
  character into a line that already had a buffer wrote one byte past the end --
  into the next heap allocation, which is usually the *following line's* buffer.
  That is "open a .txt, insert a character, and the highlighting below it shifts".
  The buffer array now comes with a capacity array that the highlighter keeps true
  (`nb_hl_lines()` grows a buffer that is too small), so no caller can get it wrong;
* after a **newline** every line below the edit is a *different line*, so the
  recorded block states are meaningless from there down (`nb_hl_shift()`), and the
  style buffers below have to go with them;
* `stop_when_stable` means "the later lines' colours are still correct" -- which is
  only true if they were coloured before.  The draw path no longer stops early, so a
  screenful inside one long comment is coloured line by line instead of leaving
  everything below the first line in the plain text colour (the test reproduces that
  as "12 of the visible lines are not comment-coloured").

### 6.7 Three more from the calculator: the stripper, the workspace list, the palette

**The stripper ate half a log message.**  `tools/strip_prose.py` decides what is a
docstring from the token stream, and it accepted a string followed by either
`NEWLINE` or `NL`.  A docstring is followed by `NEWLINE` (it ends a statement); a
fragment of an *implicitly concatenated* string is followed by `NL` (the line break
is inside brackets).  So this lost its second line:

```python
        log('[hook] keeping the image at 0x%08X mapped (hook calls in '
            'flight: %r, slot still armed: %r) until the next load' % (...))
```

and the deployed launcher died on the exit path with
`TypeError: not all arguments converted during string formatting`.  Two guards now
exist: the emulator suite **runs the stripped launcher** (the file `make deploy`
ships, rather than the commented source), and a check re-parses both with `ast` and
requires every string literal that is not a docstring to survive.

**Every workspace showed nbCode's own files.**  The launch file carried ONE list --
the default workspace's -- so choosing `C:\DATA` in the start screen listed the
application folder.  Format v3 carries **one list per workspace** (`nb_cfg.h`), with
the names in a shared 16 KB pool so eight workspaces still fit in the image; the
launcher lists every workspace through the firmware's finder, `nb_file_select()`
really switches which range the accessors read, and the browser opens a file by
joining the name to *its own* workspace.

**"The highlighting is there but I cannot read it."**  True, and measurable: the
first palette had comments at 3.7:1, numbers at 3.7:1, library calls at 4.2:1, line
numbers at 2.5:1, the status bar at 3.9:1 and the selected row at 3.7:1 against
their backgrounds.

**And then the over-correction: "it all looks like black text."**  Chasing contrast
alone drives every colour toward black, which is invisible for the opposite reason.
Two measured rules now, not one: every code class is at least **4.5:1** against its
background *and* at least **0.45 HSV saturation**, with the hues kept far apart (red
keywords, teal control flow, magenta numbers, green strings, blue operators, orange
library calls, purple directives).  Comments and the line-number gutter are the two
that are meant to sit back, and they are exempt from the saturation rule.
`tests/test_host.c` computes both numbers from the built theme, for both themes, and
fails if either drops below its floor.

### 6.8 Folders in the browser, and a start-up that is not paid twice

**The file list can show folders and step into them.**  The launcher is the only
side that can list a directory (the image has no directory API at all), so the
launch file now carries a **table of directories** (format v4): each one's path, its
parent, and its entries -- where a *folder* row carries the index of the directory
it points at.  The launcher lists each workspace and the folders inside it (two
levels deep, within a budget it logs), so `C:\DATA` shows its `.hpappdir` folders
and `ENTER` on one shows *its* files; `ESC` goes up one folder before it leaves for
the workspace list.  Opening a file also points the browser at that file's folder,
so `F1` from the editor lands where the user was.

**Start-up is paid once per session, not once per run.**  Loading an image -- the
ELF copied into the firmware heap and its relocations applied -- is the expensive
phase (163 of a 189 round-trip start-up in the emulator, and a hundred thousand
trips if the firmware refuses to do the copying).  None of it changes between runs
of the same file, so the block stays **mapped** and a second run re-enters it: 26
round trips in total, 1 for the load phase.  Keeping it mapped is also the *safe*
choice -- the OS's input thread can be inside the hook when a session ends, and the
image may not be freed under it -- and `main.unload()` (or loading a different
image) is what gives the memory back.

**The start-up cost is printed per phase**, because there is no clock on this
calculator (its `get_time` reboots a G1) and the cost that matters is the debug
round trip:

```
[cost] settings + workspace list      0 round trip(s)
[cost] listing every workspace        0 round trip(s)
[cost] image load / reuse             1 round trip(s)
[cost] the editor session            13 round trip(s)
[cost] teardown                      12 round trip(s)
[cost] start-up                      26 round trip(s) in total
```

The folder listing also got cheaper: it stops at the first attribute mask that
lists the directory instead of asking four times (four times the calls, and four
copies of every name -- which was its own bug once).

### 6.9 The load that was 121,875 round trips, and touch

**The firmware loads the image now, not Python.**  The log made the diagnosis
trivial: `[cost] image load / reuse 121875 round trip(s)` -- exactly 476 KB divided
by four, i.e. the launcher copying the image **one word per debug round trip**
because the firmware's `fread` refused to take a destination pointer from Python.
The answer is not a faster loop, it is not doing the copying there at all:

* the **in-firmware loader** -- primeLua's `LOADER_CODE`, 690 bytes of ARM code
  uploaded into the firmware heap -- does the whole job in ARM code (fopen, read
  the header, copy each `PT_LOAD`, apply the `R_ARM_RELATIVE` relocations) and
  returns the entry address.  One call.  It is the loader primeLua has been
  running on this hardware all along, which is why it is copied rather than
  written, and it is uploaded once per session;
* the image is then **verified anyway** (the entry must be inside the image, and
  the first bytes of the first segment must match the file), so a blob that did
  nothing falls back to the slower paths instead of producing an image that
  crashes later;
* Python must **not** apply the relocations itself in that case -- adding the base
  twice is an image that starts and jumps into nowhere -- so the log says which
  side did them.

**197 round trips for the first start-up, 1 for a reuse** (was 121,875 + reuse).

**Touch, in the editor.**  Three gestures, all on the real touch path:

* **drag scrolls by pixels, both ways** -- the text follows the finger on the
  vertical axis (`yoff` is the sub-line offset, and the body draws one extra row so
  the sliver at the bottom is real) and on the horizontal one (`left` frames plus
  `xoff` pixels).  Both are clamped to the content, so nothing can be dragged into
  blank space, and §6.11 has the details;
* **a long press starts a selection** (12 frames, about 0.4 s, and a finger that
  has moved more than 5 px is dragging instead), and a further drag extends it;
  the selection is drawn behind the text, `F5` cuts it, and typing or backspace
  replaces it.  Cutting a selection that is larger than one undo record
  (`NB_UNDO_TEXT`) says so instead of restoring 96 bytes and calling it undone;
* **a tap puts the caret** where the finger is.

**The UI is in Chinese** (`工作区`, `已打开`, `退出前保存吗？`, …).  The font already
carries GB2312, and the host tests check that a Chinese UI string really draws ink
across its full-width cells -- not just that a file's Chinese text does.  The
**log** stays English: it is read while debugging a start-up and mirrors the C
identifiers.

### 6.10 The five letters that could not be typed, and the codec the device does not have

**"a, b, c, d and e cannot be typed"** was not a key that had stopped working: the
mapper's alpha table was a QWERTY guess.  It ran `q w e / r t y / u i o / a s d /
x c v / z b / m n p / l j k / h g f` down the keypad in scan order, and -- the part
that made five letters *unreachable* rather than merely misplaced -- it had no
entry at all for the five device codes `0x41..0x45`, which are the keys whose only
character is their alpha letter (the calculator's vars/toolbox/units/define/a b/c
keys).  The letters that ARE on the keypad were in the wrong places too.

The printed letters are recoverable from the reference implementation, and the
derivation is worth writing down, because otherwise it gets guessed again:

* the Python **PrimeCode** (`primecode/keys.py`) lists a letter per PPL `GETKEY`
  id, and its own comment says the alpha layer is what the keys are printed with;
* the device scan codes come from primetcc's `rt/hp_input.h`, which is
  hardware-verified and names the 7 key "7/Q" (`0x51`) -- so `GETKEY 32 = 'q'`
  fixes one whole row;
* the rest of the number pad lines up one for one with the device codes
  `0x47..0x5A` in firmware scan order (sin/cos/tan/ln/log/x²/± at `0x47..0x4D`,
  then 9/÷/6/×, then 3/−/0/./space/+);
* the shift layer confirms it independently wherever it can be checked against
  the real machine: `GETKEY 24` is `ln` and shifts to `exp()`, `26` is `x²` and
  shifts to `sqrt()`, `20` is `x^y` and shifts to `NTHROOT`, `18` shifts to the
  degree-minute-second symbol.

So `0x41..0x45` carry **a b c d e**, `0x46..0x50` carry **f..p**, and the keypad
carries `q r s / u v w / y z #`.  All 32 keys and their letters are pinned by
`tests/test_host.c`, which fails if the table drifts.

**ALPHA and SHIFT are now the two second-level keys, as on the device:**

| press | what the next keys type |
|---|---|
| `ALPHA` once | one letter, then the layer is off |
| `ALPHA` twice | **locked**: letters until `ALPHA` is pressed a third time |
| `SHIFT`+`ALPHA` | the same cycle in capitals |
| `SHIFT` | the shifted character: punctuation, and `SHIFT`+`ON` a tab |

Alpha wins over shift on a key that carries a letter (that is how the calculator
works, and primeCode cannot reach its shift layer with alpha set either), and the
status line names the layer that is armed -- `字母` / `字母锁` / `大写` -- because
an unmarked locked alpha is a user typing `7` and getting `q`.  The old version
latched alpha on the first press and never turned it off, and its uppercase branch
was dead code (shift was tested first), so capitals were unreachable entirely.
Together the three layers reach exactly the 95 printable ASCII bytes, which is now
a test rather than a hope.

**The other report -- "UnicodeError at line 1359 after exiting" -- is the
device's codec.**  MicroPython does not implement the non-strict error handlers:
`bytes.decode('utf-8', 'replace')` raises there whatever the data is.  The
launcher was parsing the image's log ring with exactly that, and the ring's status
window is the **last 64 bytes** -- which, now that nbCode's messages are Chinese
(three bytes each), routinely opens in the middle of a character.  So the editor
exited and the launcher died before it could report anything.

The launcher now owns its codec: `decode_bytes()` tries UTF-8, then latin-1, then
a per-byte conversion, and never raises; `utf8_bytes()` writes UTF-8 by hand; the
ring is decoded line by line, so a window that starts mid-character damages one
line instead of the whole read; and no call anywhere passes an `errors=` argument.
Both halves are enforced in `tests/test_emu.py`: the suite runs the launcher with
a `bytes` whose decoder behaves like the device's (a non-strict handler is a
`UnicodeError`), and an AST check refuses a non-strict handler in the deployed
source.

### 6.11 Three more from the calculator: half a character, a view that walked away, and a loud console

**"Selecting Chinese leaves the last character half highlighted."**  The selection
was measured in FRAMES times 8 pixels, and a full-width character is **two**
frames.  The highlight now walks the decoded characters and uses each one's own
advance (8 or 16 px), so a character that any selected byte touches is covered
whole -- including when the selection's end falls inside one, which is what the
user saw.  The test measures the highlighted span in the framebuffer: 16 px for one
Chinese character, 32 px for two.

The same code had an off-by-one that this fix would have left in place: the
predicate was inclusive (`byte_off <= c1`) while `nb_buf_delete_range` takes a
half-open range `[c0, c1)`.  So the highlight showed one character MORE than the
cut removed.  It is half-open now, and a test cuts a selection and compares the
resulting line.

**"With 18 lines the cursor stops moving up at the last line -- and on the first
line the view still scrolls up."**  The view is stored as `top_line` plus `yoff`
(the remainder) and was clamped one field at a time: the drag added to `yoff`
without a limit, so at the top it went NEGATIVE (a gap opened above line 1) and at
the bottom it grew past the body height (the text sat above its own end).  Worse,
the caret-follow compared LINE NUMBERS -- `if (cur_line < top_line) top_line =
cur_line` -- so once the offset had escaped, the caret's line could be inside the
window's line range while being off the glass, and the view never came back.  That
is the "it only moves down" report.

`top_line`+`yoff` and `left`+`xoff` are now treated as ONE pixel number per axis
(`scroll_px`/`set_scroll_px`, `scroll_left_px`/`set_scroll_left_px`) and clamped to
`[0, content - body]`, and the follow works in pixels too.  Two consequences worth
stating: the caret can never be off the glass after a key, and a patch of blank
space can never be revealed at either end.

**The follow had a second bug the fix exposed:** it ran on every draw, so it
dragged the view back to the caret after every finger movement -- with the caret on
line 1 that made vertical dragging do nothing at all.  It now runs only when the
caret (or the line count) has changed since the last time it ran, which is what
"the window follows the caret" has to mean: a view the user dragged is left where
they put it, and the next key press or tap brings the caret back into view.

**"Dragging should move the view sideways by pixels too."**  It does now: the
horizontal scroll is `left` (frames) plus `xoff` (0..7 px), the text is drawn at
`gutter - xoff` (the gutter is painted afterwards, so it keeps its column), the
tap/selection mapping goes through the same pixel offset, and the drag is limited
to the longest VISIBLE line -- measured by the draw, which decodes those lines
anyway -- so it stops at the end of the text instead of uncovering blank space.

**"Do not print logs at start-up and exit."**  The launcher's log FILE is unchanged
(every line, including the image's, still goes to `nbcode.log`, which is what a
crash is diagnosed from), but the console is quiet: `log()` writes the file and
says nothing, and `alert()` is the version a genuine failure uses
(`[load] FAIL: ...`, a refused `fopen`, invalid JSON, an editor that did not
return).  An empty file named **`verbose`** next to `main.py` brings the whole
`[load]`/`[cost]`/`[hook]` stream back, and `main.probe()` always prints, because
that entry point exists to be read.  The emulator suite asserts that a normal run
prints NOTHING at all on the way in or out.

### 6.12 The title bar draws last, a shift that stays down, and the panels the keypad cannot reach

**The header is now painted AFTER the body.**  A pixel-scrolled top line is drawn
even when it is half way out of the body (that sliver is what makes the scroll
look right), so a header drawn first had the code running through the title.  Two
tests hold it: the header band is pixel-compared between a normal draw and one
where the top line is scrolled up 8 px, and they have to be identical.

**SHIFT IS A LATCH, not a hold.**  Press it and it stays on until the next key
uses it (press it again to turn it off); the status line says `大写` while it is
armed.  This is what primeCode does -- `k_shi = (k_shi + 1) % 2`, cleared by the
key it was pressed for -- and on a calculator with a touch screen it is the only
workable spelling: a modifier that has to be held while a second finger presses
another key cannot be used with one hand.  ALPHA keeps its own cycle (once /
locked / off, `shift`+`alpha` for capitals), which is primeCode's machine too.

**The editing chords are primeCode's as well** (`keys.py`, GETKEY ids in
brackets): `shift`+`left` [7] goes to the start of the line, `shift`+`right` [8] to
its end, `shift`+`BACKSPACE` [19] deletes forward.  The plain arrows and backspace
are unchanged, and `shift`+`alpha`/alpha still give the letter layers of §6.10.

**SHIFT+A opens the character table** -- the keypad has 32 character keys and they
cannot type `°`, `×`, `《` or `①`.  The table is a grid the user taps
(`nb_ui_overlay.c`), and it holds:

* printable ASCII, `0x20..0x7E`, in order (so `A` is index 33, which is what the
  test taps);
* the common symbols: the Latin-1 marks and money (`° ± × ÷ ² ³ ¼ ½ ¥ © §`), the
  Greek a formula is written in (`α β π Ω …`), stars and circled numbers
  (`★ ☆ ① ②`), and the CJK punctuation a Chinese sentence needs
  (`。 ， 、 《》 「」 【】 （）`) plus its full-width ASCII forms.

TWO RULES MAKE IT USABLE RATHER THAN A NOVELTY.  First, a codepoint the font
cannot draw is NOT offered -- the palette is filtered through `nb_font_glyph`, so
there are no boxes to tap (the font has no `√ ∞ ≠ ≤ → • … — “ ”` and no
box-drawing characters, and the table simply does not pretend otherwise).
Second, the insertion goes through the **buffer's encoding**: a GB2312 file gets
the GB2312 pair (the test taps `。` into a GB2312 buffer and checks the two bytes),
a UTF-8 file gets UTF-8.  A table that always wrote UTF-8 would corrupt every file
that is not.

**A see-through scrollbar, dragged for pixel-level scrolling.**  It appears on the
right only when the file is taller than the body, is painted with `nb_gfx_blend`
(the panel has no hardware alpha -- the top byte of an ARGB pixel is ignored by
the LCD, so "transparent" has to be mixed in the back buffer), and its geometry
comes from the SAME `scroll_px` the finger drag uses, so the thumb cannot drift
away from the text.  Its touch target is 14 px wide, because a 4 px bar is not a
finger, and a scrub never moves the caret or starts a selection.

**The file type moved to the status bar's right hand end** (next to the layer
indicator), because the header's right hand end is now **the hamburger menu**:
tapping it drops a panel with 保存 / 另存为 / 文件信息 / 跳转… .

* 保存 is the same save F3 does;
* 另存为 asks for a name in the existing one-line input (pre-filled with the
  current one) and writes into the same folder; the language follows a changed
  extension, so a `note.c` saved as `note.py` is coloured as Python from then on;
* 文件信息 shows the name, the path, the language, the encoding, the line and byte
  counts, whether it is modified, the caret line and the scroll position;
* 跳转… asks for a line number and puts the caret there, fully on the glass.

A panel is MODAL in the strict sense: `nb_ui.c` offers every touch and every key
to the open panel first, and a panel that answers 1 has eaten the event.  A tap
that picked a character and also moved the caret behind the panel is a bug nobody
sees until the text is in the wrong place, which is why there is one place that
decides.  `esc`, `ON` and F1 close a panel.

### 6.13 A draw no longer touches the panel until it is presented, and a selection that can be longer than one screen

**THE FLICKER WAS THE ABSENCE OF A BACK BUFFER.**  `port/nb_screen.c` used to draw
straight into the firmware's own pixels, on the reasoning that a second 320x240x4
buffer is 307 KB of a firmware heap this image already uses.  That reasoning is
right about the memory and wrong about the screen: every frame CLEARS the body and
redraws it in place, so the panel shows the erase before it shows the text, and a
drag of the code flickers the whole window.

The image now allocates a **shadow buffer** with the firmware's `calloc` (one call,
once per session) and `nb_screen_present()` copies it into the firmware's pixels in
one word-at-a-time pass, so the panel only ever sees a complete frame.  The rules:

* **the allocation is optional**: if the firmware refuses it, the log says
  `screen: no shadow buffer (drawing direct)` and the editor keeps working exactly
  as before -- flickering, but working. A screen that goes black because a
  cosmetic buffer could not be found would be a much worse trade;
* **it is released at exit** (`nb_screen_free()` in the teardown path) and
  re-allocated on the next run: the calculator wants that memory back while nbCode
  is not running, and the resident image gets it again for 1 extra round trip;
* **the host port models it** (`back` and `fb` are separate arrays, `present`
  copies one to the other), which is what lets a host test see the property that
  matters: **a draw leaves the panel untouched until it is presented**.  Without
  that test the two buffers could quietly collapse into one again;
* the emulator suite covers the other end: after the run, all **76,800** pixels of
  the *firmware's* framebuffer are non-black, i.e. a whole frame went through the
  blit.  (Before the shadow, that check passed on a half-drawn screen.)

**A SELECTION DRAG NOW SCROLLS AT THE EDGES.**  A finger that reaches the edge of
the glass -- or leaves it: the digitizer keeps reporting the last position it had
and the input hook drops the contact once it is invalid -- has to keep the text
moving, or a selection can never be longer than one screenful.  The caret-follow
does not do this (it follows the *caret*, and a selection drag does not move the
caret), so the editor now has a per-frame edge scroll of its own while a selection
is being dragged: **12 px** from the body's edge starts it, **8 px per frame**
(about 30 Hz) moves it, the selection's end is re-read from the finger's (clamped)
position after every step, and it stops at the ends of the file.

It also stops when the finger stops reporting for 150 frames.  A finger lifted off
the glass *outside* the screen can take its release event with it, and a scroll
that runs for ever is worse than one that gives up: the idle counter is reset by
any touch event, so the next real contact resumes it.  The test sets that counter
directly rather than waiting 150 frames -- really waiting would scroll to the end of
the file first, which hides what is being tested.

### 6.14 The keys are primeCode's, the table is a cursor, and the messages are toasts

**THE KEY LAYERS ARE primeCode's `norm`/`shi` NOW.**  Pressing the `sin` key types
`sin()`, not `=`; the bracket key types a **pair**; `shift`+`sin` is `asin()`,
`shift`+`x²` is `sqrt()`, `shift`+`ln` is `exp()`, `shift`+`8` is `{}`,
`shift`+`5` is `[]`.  **And after inserting a snippet that ends in `) ] } " '`
the caret lands INSIDE it** -- primeCode's `if adde[-1] in ')]}"': gb -= 1`, which
is the whole difference between a bracket key that helps and one that has to be
corrected every time.  A *lone* closing character does not move the caret (that
rule is for snippets: more than one character).

Where this deliberately differs from primeCode: its shift layer leaves six keys
empty or holding something a C file cannot use, and it has no way at all to type
`$ & < > ? @ \ | ~`.  Those six keys carry the punctuation C needs instead, so
**all 95 printable bytes stay reachable** -- `tests/test_host.c` walks every layer
and insists on it.  The layers are strings now (`nb_key_meaning.ins`), not
characters, since "sin()" is six bytes.

**THE CHARACTER TABLE IS A CURSOR, NOT A TAP.**  A tap (or an arrow key) MOVES the
cursor, the title row shows **the character and its codepoint** (`U+3002  。`), and
**ENTER inserts it and closes the table**; `esc`/`ON`/F1 close it without
inserting.  A tap no longer inserts: a wrong character used to be in the file
before it could be read.  The panel says "字符表" and the selection, and nothing
else (no hint line, no count).

TWO BUGS CAME OUT OF THIS, both worth naming:

* **the panel titles were invisible.**  They were drawn in `head_fg` on a body
  background, and `head_fg` is the colour that belongs with `head_bg` -- a dark bar
  with white text -- so on the light panel "字符表" and "按键帮助" rendered as
  nothing.  Every panel now uses the pair that goes with the surface it is drawn
  on, the theme test checks those pairs, and the panels' titles have ink checks on
  the framebuffer (a theme invariant cannot catch this one: in the dark theme the
  same two colours are perfectly readable);
* **a panel that does not appear until the next key press** (what was reported for
  the table).  The dirty flag was set when it opened, so the frame loop *should*
  have drawn it -- and with the invisible title the table looked empty until
  something else forced a repaint.  Both are fixed, and `nb_ui` now gives a panel
  a few frames of guaranteed redraw when it opens rather than relying on one flag
  being set on the right path.

**THE MESSAGES ARE TOASTS.**  `nb_ui_message()` starts a countdown (30 frames,
about a second; 60 for a failure, which is the one message the user may need to
read) and the status row is given back when it runs out.  This also fixed a
quieter complaint: the message line used to fill the status row *unconditionally*,
so the position/file type/layer indicators -- and the browser's own hints -- were
covered for the whole session, message or no message.

**A TAP ON THE HEADER IS NOT A TAP ON THE TEXT.**  Choosing 保存 from the menu
moved the code, and this is why: the tap's *release* arrives after the panel has
closed, the editor saw no press for it, and the tap handler CLAMPS Y into the body
-- so a tap on the title bar put the caret on the first visible line and the
caret-follow scrolled the window to it.  Two rules now: only the body is text (a
press on the header or status bar is not a press on a line), and a press that a
panel eats ends the gesture, so its release cannot become a tap.

### 6.15 A folder that is not nbCode, a MENU key, and a browser you can touch

**"CHOOSING ANY OTHER .hpappdir ALWAYS TURNS INTO nbcode.hpappdir."**  This one
was not a display bug, it was the launcher's directory table running out:

* every directory the launcher lists costs a slot (`NB_CFG_MAX_DIRS`, 24).  The
  workspaces are added first and the folder walk (breadth first, depth 2) spends
  the rest -- and `C:\DATA` alone has more `.hpappdir` folders than that;
* a folder ROW whose directory never got a slot was written with **`bytes = 0`**,
  and the image reads `bytes` as "the directory index to show";
* index 0 is the first workspace, which is the application folder.  So every
  folder past the budget opened **nbCode.hpappdir's own files** -- and because the
  finder lists folders in name order, the folders that still worked were the first
  few, which is exactly why one of them ("&...") looked fine;
* the same fallback existed for workspaces: `ws_dir[i]` defaulted to directory 0
  when a workspace's path was not in the table.

The rules now: **a row or a workspace with no directory shows NOTHING** (`bytes =
NB_CFG_NO_DIR`, `ws_dir[i] = NB_CFG_NO_DIR`, and `nb_file_select` clears the list
instead of leaving the previous folder up), the table holds 64 directories, the
paths are compared case-insensitively with a trailing separator forgiven, and the
image logs the workspace -> directory mapping so a device log answers this
question directly.  A finger that taps such a row gets "无法打开该文件夹" instead
of someone else's files.

The test builds the launch file BY HAND and parses it with the **real parser**
(the host model had always built its own list, which is why this survived), in the
calculator's own shape: several `.hpappdir` folders under `C:\DATA` listed both as
workspaces and as folder rows, one workspace missing from the table, and one
folder row with no directory.

**THE MENU KEY, AND NO MORE F1..F6.**  The calculator has Apps, Home, Symb, Plot,
Num, Help, View, Menu, Esc and CAS -- there is no function-key row, so the old
F1..F6 bindings were keys the user could not press.  Two of those keys now have
their real jobs: **MENU** opens the current screen's popup and **HELP** opens the
key reference (which every screen shares now).

The popup is one widget with a per-screen item list, navigated with the arrow keys
or a finger and fired with ENTER or a tap:

| screen | menu |
|---|---|
| workspace picker | 进入 · 刷新 |
| file list | 打开 · 新建文件 · 重命名 · 删除 · 刷新 |
| editor | 保存 · 另存为 · 撤销 · 复制整行 · 剪切整行 · 剪切选区 · 文件信息 · 跳转… |

**DEL (PPL 19) DELETES A FILE.**  The key the keyboard labels with a backspace
arrow is what a browser should use, and it asks first (`nb_ui_prompt_delete`).
**RENAME IS A REAL RENAME**: the calculator's API in this workspace has no rename
call, so the bytes are read, written under the new name and the old file removed --
which also works across the two file systems (the firmware's and `uio`'s).

**THE BROWSER IS TOUCH-ABLE.**  A tap opens the row under the finger (a file opens,
a folder steps in, a workspace is entered) rather than moving a selection that then
needs ENTER; a drag scrolls the list **by pixels**, with the same one-pixel-number
model as the editor (`top` + `list_yoff`), clamped to the rows so the list cannot
be dragged into blank space.

### 6.16 A modern interface, and the numbers behind it

The chrome was FileManager's: a dark blue header bar with white text, a full-width
blue selection, coloured syntax on white.  It is now a **flat surface-and-ink
scheme with one accent colour**, in both themes, and every pair is measured rather
than eyeballed:

| role | light | dark | note |
|---|---|---|---|
| page | `#FFFFFF` | `#0D1117` | the code and the lists |
| chrome | `#F6F8FA` | `#161B22` | header, status bar, every panel |
| ink | `#1F2328` | `#E6EDF3` | 15.8:1 / 16.0:1 |
| secondary | `#59636E` | `#8B949E` | hints, sizes, the right of the header |
| divider | `#D8DEE4` | `#21262D` | 1.36:1 / 1.24:1 -- a hairline, not a bar |
| **accent** | `#0969DA` | `#2F81F7` | the caret, the scrollbar's thumb, the header's underline, the edge of the selected row |
| selection | `#E3EEFF` | `#16263D` | a tint, with the syntax colours still on it |

WHAT "MODERN" MEANS IN PIXELS, since a 320x240 panel does not have room for
decoration:

* **the header is a quiet bar with one accent hairline under it** instead of a dark
  block -- the separator is a 1 px line, so the title keeps its pixels;
* **panels (menus, information, the palette, the help, the question) share one
  look**: surface, hairline border, a 2 px accent edge on the left
  (`nb_ui_panel()`), so they read as the same interface rather than as five
  different ones;
* **the selected row is a tint with an accent edge**, not a saturated blue bar:
  the syntax colours stay readable on it, which a full blue makes impossible;
* **the gutter has a hairline on its right**, the status bar one on its top;
* **the scrollbar's thumb is the accent**, drawn see-through over the text.

THE RULES THE PALETTE IS CHOSEN BY, all enforced by `tests/test_host.c`:

* code classes: **contrast >= 4.5:1** on the page *and* **HSV saturation >= 0.45**
  (contrast alone drives everything to black, and then "it all looks like black
  text" -- which is exactly what a previous round fixed);
* the same colours are also read **on the selection tint (>= 4.2:1), on the
  current line (>= 4.5) and on a matched bracket (>= 4.5)**.  The selection floor
  is 4.2 on purpose and the reason is written down: a selection nobody can see is
  worse than one whose colours are 4.2:1;
* comments and the gutter are the two that are meant to sit back, and they are the
  only ones exempt from the saturation rule;
* chrome: header, status bar, panels, the selected row, the accent -- each >=
  4.5:1 where text is drawn on it, >= 3:1 for the accent itself;
* the divider has to be **visible (>= 1.15:1) and quiet (<= 2.2:1)** -- the range
  is the rule, because both extremes have been shipped by accident.

`make test-host -v` prints the browser and the editor as ASCII art, one character
per colour with a legend, which is how the layout is reviewed without the
calculator in hand.

### 6.17 Completion, bracket pairs, and four things the calculator asked for

**CODE COMPLETION** (`nb_complete.c`).  Type two characters of a word and a six
row popup appears under the caret, offering

* the words of the language profile (keywords, control flow, library names --
  `nb_highlight.c` already carries them for the colouring, so a language brings its
  vocabulary along for free), and
* the identifiers already in the file, which is what makes a long name in the same
  buffer reachable in three keys.

It is filtered by what is typed, de-duplicated, sorted shortest first, and the
typed part is drawn in the **muted** tone so the eye sees what would be ADDED.
UP/DOWN move, ENTER or TAB or a tap accepts, ESC closes *only the popup*, and any
other key closes it and does its own job.  Two rules keep it out of the way: it
never opens on a single character (the list would be the whole file) and it does
not re-open for the word it just inserted.  `setting.json` can turn it off with
`"complete": false`.

**THE MATCHING BRACKETS ARE A DIFFERENT COLOUR.**  The bracket under the caret (or
the one just before it) is matched -- nesting of the same kind included, and the
scan is bounded so an unclosed brace cannot cost the frame a pass over the file --
and the pair is drawn with a box behind it *and* its glyphs in the **accent**, the
colour this interface already uses for "where you are".  A bracket with no partner
lights nothing up.

**AND FOUR SMALLER ONES:**

* **the browser draws its header LAST.**  A half scrolled row is drawn above the
  list on purpose (that sliver is what makes the pixel scroll look right), so the
  file names were running into the title bar;
* **the file list has the scrollbar too** when the folder is taller than the
  screen -- the same see-through bar, dragged with the same code: the scrollbar now
  asks *whatever screen is up* for its scroll position (`nb_ui_scroll_px()`),
  instead of the editor's;
* **a panel's title is a muted grey** (`nb_ui_menu_open("文件", ...)`): it names the
  panel the user has already opened, so it is decorative rather than information,
  and it is the one text tone exempt from the contrast rule;
* **the status bar's hints no longer name the MENU key** (the key says so itself,
  and 40 columns is not much room), and the character table's cursor cell lost its
  accent edge -- inside a 16 px grid that edge covers half the glyph.

**AND A REAL BUG THE NEW TESTS FOUND.**  `nb_buf_delete_range()` -- the function
behind "cut the selection" and "delete lines" -- ran its multi-line loop one line
TOO MANY and then joined the line AFTER the range, which for a range that ended the
file read past the end of the line array.  Cutting a selection across lines
corrupted the heap ("corrupted double-linked list" from glibc, in a test that only
wanted to check the bracket colours).  The three pieces (the first line's tail, the
lines strictly between, the last line's head plus the newline) are now each removed
once, the byte count is checked against what `nb_buf_to_raw()` writes, and there
are tests for a range that ends the file, a range that does not, and the cursor.

### 6.18 Folding, a symbol panel, and a start-up you can watch

**CODE FOLDING: TAP THE LINE NUMBER.**  The gutter is the margin, so the margin is
where the marker lives: a small `[-]`/`[+]` block (drawn as pixels, 7 px wide) on
every line that CAN be folded, and a tap there folds or unfolds it -- without moving
the caret, because the line the user is working on is none of the fold's business.
A folded line ends with a muted `...`, the lines below move up into the gap, and

* the renderer walks *visible* lines (a screen row is no longer a line number);
* UP/DOWN **step over** hidden lines, and so does the caret-follow: "the caret is on
  the glass" is now asked as "how many VISIBLE lines are between the top of the
  window and the caret" (`nb_ui_editor_row_of`);
* an edit that adds or removes a line **drops the folds**.  A fold is stored as a
  line number; renumbering a list of ranges on every keystroke gets it wrong once
  and then hides lines the user cannot see, so one tap brings them back.

WHAT CAN BE FOLDED, in three shapes: **brackets** (a line whose opener closes on a
later line -- C, Java, JS, and Lua tables and Python literals for free), **keywords**
(Lua and PPL: `function`, `do`, `then`, `repeat`, `BEGIN` … to the matching
`end`/`END`/`until`, counting nesting), and **indentation** (Python: a line ending in
`:` folds the lines indented further than it, blank lines included).  A keyword
inside a string fools the keyword scan, and that is accepted and documented: this is
a folding aid on a 40 column screen, not a parser, and being wrong costs one tap.
The bracket matcher itself is now shared with the editor's pair highlighting
(`nb_fold_match`).

**SHIFT+6 IS THE QUICK SYMBOL PANEL.**  Nine cells, each with a **digit shortcut**
(the number keys are unshifted, so it is one key) and a tap target, and the list
follows the file's language:

| language | the nine cells |
|---|---|
| C / text | `> < = == != >= <= && ||` |
| Lua | `== ~= >= <= and or not .. #` |
| Python | `== != >= <= and or not ** //` |
| PPL | `:= == ≠ ≥ AND OR NOT MOD →` |

It closes after inserting: this is a quick panel, not a keyboard.  Its key was the
one free key whose shifted character is also reachable elsewhere (`|` moved to
`shift`+the `d` key), which is what keeps **all 95 printable bytes** reachable --
the coverage test still walks every layer and insists on it.

**THE LAUNCHER SHOWS ITS PROGRESS.**  A load is a second or two of a blank screen,
and the launcher is the only side that knows what is happening, so it now prints six
short lines as it goes:

```
nbCode 1/6: finding the application folder
nbCode 2/6: reading setting.json
nbCode 3/6: listing the workspaces and their folders
nbCode 4/6: loading nbCode.elf into the firmware
nbCode 5/6: starting the editor
nbCode 6/6: closing down
```

They are printed whether or not the console is in its quiet default (they are not a
log -- they are what the user is waiting for) and they go to the durable log as
`[progress] …`, so a start-up that dies halfway leaves the last step it reached.
The emulator suite runs a real start-up and checks the lines come out in order.

**AND ONE BUG THAT CAME OUT OF THAT:** the image's durable log kept only the last
**2 KB** and, once the file was bigger, **wiped it** rather than trimming it -- so a
slightly longer start-up silently erased the lines a crash is diagnosed from (the
progress lines were enough to trigger it).  `nb_logfile()` now keeps the last 16 KB
of the tail.

### 6.19 The keypad's own order, a title bar with air under it, and a fold that does not move the code

**THE SYMBOL PANEL'S SHORTCUTS FOLLOW THE KEYPAD.**  The nine cells of the `shift`+6
panel were numbered 1..9 in reading order, which assumes the phone layout.  This
calculator's digit keys are **7 8 9 on the top row** and **1 2 3 on the bottom one**,
so the top left *key* is a 7 -- and it picked the first symbol instead of the
seventh.  The cells keep their order and the KEYS changed (`nb_ui_symbol_key`): the
digit drawn in a cell is the key that fires it, so the shortcut is where the finger
already is.  The old test that pressed the `6` key still passes and still inserts
`>=`, because 6 is the middle right cell under either numbering -- which is exactly
why the mistake survived it, and why the new checks walk the whole grid.

**A TITLE BAR WITH AIR UNDER IT.**  The header is now **18 px** instead of 16
(`NB_HEAD_H`), the title is drawn one pixel down from the top, and the accent hairline
that separates the bar from the body sits on the bar's **last** row: it used to be
drawn at `y = 15`, which is the bottom row of an 8x16 glyph, so every title with a
descender (g, j, p, q, y) had the rule through its feet.  The body starts at y = 18
and the renderer still draws a partial line at the bottom of it, so a screenful is
still a screenful.

**A FOLD NO LONGER MOVES THE CODE.**  Folding is a *list of line ranges*, and the
window was positioned by writing a **line number** into a pixel scroll
(`set_scroll_px(line * LINE_H)`): with a folded block, a line number and a screen row
are two different numbers, so folding something above the caret moved the whole view
by the height of the fold -- and the scrollbar's maximum came from the *unfolded*
line count, so the window was allowed to sit past the end of the file and the last
lines floated above a band of blank glass.  The vertical scroll is now stated in
**rows of visible lines**:

* `vis_row_of(line)` is that line's row, counting only visible lines -- O(number of
  folds), not O(lines), because a fold is a range and only its overlap with the lines
  above matters;
* the window's top is always a **visible** line (`line_visible_from`), so the row the
  gestures count and the row the renderer draws are the same row;
* `max_scroll_px()` comes from the visible count, and the draw **clamps** it, so a
  fold that shortens the file pulls the window back in;
* the caret-follow is three lines of arithmetic (`set_scroll_px(caret_row * LINE_H)`)
  instead of a loop that walked line numbers while asking about rows.

The fold list is also kept **disjoint** now (`nb_fold_toggle` drops the folds a new
one swallows): the row arithmetic adds the ranges up, and one line hidden by two
folds would be counted twice -- a drift of one row per overlapped line, for a state
the user cannot see.

**THE GUTTER WIDENS WITH THE FILE.**  The fold marker owns `x = 3..10` and the line
number is right aligned six pixels short of the divider, so a *fixed* 36 px column put
the two on top of each other from line 1000 on -- and at 10000 the number started at a
negative x and was clipped by the screen edge as well.  `gutter_width()` is now
`11 + 6 + 8 * digits`, with 36 px as the floor, so a 1200 line file gets a 49 px
gutter (and still 33 columns of code).

**THE STATUS ROW SPEAKS THE KEYBOARD'S LANGUAGE.**  The file type is **abbreviated**
(`py`, `c`, `lua`, `ppl`, `txt` -- `nb_lang_abbrev`), because that row also carries
the size, the caret position and the keyboard layer; the full name is still in the
information panel (文件信息).  The layer indicator says **`shift` / `alpha` /
`alock` / `Alock`**, the four names primeCode's keyboard uses, with a capital A for
"the letters come out in capitals": the Chinese words it replaces ("大写", "字母锁")
were neither shorter nor the name printed on the key the user has to press.  The label
is built by `nb_ui_editor_status_label()`, so the tests read it as a string instead of
guessing from pixels.

**THE LAUNCHER PRINTS THROUGH `moreprint`.**  primeLua and primetcc both print through
five helpers from a `moreprint` module the user drops next to `main.py`, and nbCode
now takes four of them with the same meanings -- `p_log` what the launcher is doing,
`p_warning` a fallback worth a look, `p_error` a failure, `p_pass` a step that worked
-- with `print()` fallbacks, so a calculator without `moreprint.py` reads exactly as
before.  `p_out` (a program's own stdout) is the one it does not take: nbCode has no
program output, because the editor owns the screen.  The emulator suite loads the
launcher twice, once with a recording `moreprint` and once without, and checks every
level.

**AND THE CRASH REPORT IN THE WORKSPACE WALK.**  A user's log ended in `'…' object has
no attribute append` on the line that pushed a folder onto the breadth-first queue.
Every receiver on that line is a list in the source, so rather than guess, the walk was
rewritten so that the line, the queue and the search are all **gone**: every directory
found is appended to the table, so the table *is* the queue and walking it forward by
index *is* breadth-first order -- and the entry being expanded is exactly the parent
index its children need.  That removes `dirs.index(parent)`, a search over the whole
table comparing dictionaries **by value** (it answers the first directory that happens
to be equal), which is the kind of thing that goes wrong only with the table the
calculator actually builds.  A folder deeper than `CFG_DIR_DEPTH` now `continue`s
instead of ending the walk, and the whole phase is wrapped: **a listing that fails may
not stop the editor** -- it logs the exception and the types involved and carries on,
because the editor is what the user asked for.

That last code path is now tested off the calculator: `list_dir_os()` resolves a
device path *under* the application folder by its tail, so the emulator's fixture has a
four-level tree and the checks read the `[run] … entr(ies)` lines the walk writes.

### 6.20 The global clipboard, and the fold that jumped to line 1

**A FOLD IS NOT A CARET MOVE.**  The report was exact: tap a line number a thousand
lines down the file and the view jumps to line 1.  The fold handler ended with
`view_line = -1`, which means "fit the window to the caret on the next draw" -- and
the caret was *still on line 1*: the user had **scrolled** to line 1000 without
moving it, which is the documented behaviour of this editor (a view the user dragged
is left exactly where they put it).  So the next frame dutifully yanked the window
back to the caret.

Folding now calls `view_anchor()` instead: the window keeps the line it has at the
top (made visible, in case the new fold swallowed it), gets pulled back in if the
file became shorter than it, and is marked as agreeing with the caret so the follow
does not fire.  Two other things came out of the same report:

* **the caret may not end up inside a fold.**  Folding the block the caret is in used
  to leave it on a hidden line, i.e. typing into something invisible; it now moves to
  the block's own line, which is the line the user just tapped;
* the regression is tested with the caret and the fold far apart (a fixture whose
  block is in the middle and a window scrolled to it), because that is the only shape
  in which the bug appears at all.

**THE CLIPBOARD IS GLOBAL NOW (`src/nb_clip.c`).**  What was there was one slot per
*buffer*: a cut, and the paste right after it.  That cannot carry a line from one
file to another and forgets a copy when the file closes.  The new one is a list of
**ten** entries, shared by every file and written to a file:

* `shift`+`VIEW` **copies** -- the selection when there is one, otherwise the
  caret's line (with its newline, so pasting gives you a line, not a fragment);
* `shift`+`MENU` opens the **panel**: ten rows, the newest first, `UP`/`DOWN` or a
  tap to pick, `ENTER`/tap to insert it at the caret, `Del` to delete one, `esc` to
  close.  The editor's own `MENU` popup gained a 剪贴板 row, because a 320x240 screen
  has no room for a legend of chords;
* the list is written to **`nbclip.txt`** next to `nbcode.log` on every change
  (magic line, then `<bytes>\n<bytes>\n` per entry -- binary safe, so an entry can
  hold newlines and Chinese), and read back at start-up: `clip entries=N` in the
  durable log is where a session says how many it recovered;
* the FILE is derived from the launch file's `logpath` (the only path the launcher
  tells the editor) -- no config format change, no version bump;
* an entry copied twice moves back to the **top** instead of appearing twice, the
  eleventh copy pushes the oldest out, and the same copy repeated changes nothing.

**AND A CUT THAT LOST WHAT IT CUT.**  Writing the copy path turned up a real bug next
to it: the menu's *cut selection* action called `nb_ui_selection_delete()` without
copying the text anywhere -- `nb_buf_delete_range()` does not fill the clipboard.
So a cut destroyed the selection.  Both the cut actions now go through
`nb_ui_editor_copy()` first, which puts the text in the global list *and* in the
buffer's own clipboard, so the paste that follows a cut works as well.

**THE HELP PANEL'S BUDGET.**  The key reference is the whole screen (a 240 pixel
display does not scroll a help page): 20 pixels of title, then `(240 - 20) / 16 = 13`
lines, and the fourteenth is dropped.  There were already fourteen -- the version
line was never drawn -- so the list was rewritten to thirteen *including* the two new
chords, with the version line as the thirteenth.

### 6.21 The finger's own drift, and a paste that rebooted the calculator

**A TAP THAT SCROLLS THE VIEW IS NOT A TAP.**  The report was "I tap a line number
to fold and the code runs around, and left/right is the most obvious; tapping the
menu does it too".  Both are the same bug, and it is in the gesture bookkeeping: a
press that the gutter, the menu icon, the completion popup or anything outside the
body CONSUMES set `touch_down = 0` and `touch_moved = 1` -- the first so the release
cannot become a caret tap, the second because the drag path only asked whether the
finger had moved.  So the tap itself armed the drag: every millimetre the finger
drifted after it went straight into `set_scroll_px`/`set_scroll_left_px` and moved
the text.  A finger always drifts, and a horizontal drift moves the text in 8-pixel
steps, which is why left/right was the obvious one.

There is now a third state, `touch_eaten`: the gesture belongs to the panel, the
gutter or the header, and **nothing** is done with it until the finger comes up --
not a caret tap, not a selection, not a drag.  The test presses, drifts six pixels
and releases (a `jitter_tap`), and with the fix removed it reproduces the report
exactly: "folding does not move the view sideways" fails with `left 100 -> 136` and
`top 11 -> 19`.

Folding also stopped touching the caret at all.  It used to move a caret that was
inside the folded block out to the block's line (so that typing could not land on a
hidden line); a fold is not a request to go anywhere, so instead **the first edit
inside a fold reveals it** (`nb_fold_reveal`) -- one toast, and the user sees where
they were.

**A PASTE COULD REBOOT THE CALCULATOR, and the reason was a line pointer.**  Every
caller in `nb_buffer.c` holds a COPY of a line pointer (`l = b->line[i]`), and
`line_reserve()` reallocated the block and **threw the new pointer away**: the
buffer went on using freed memory, the next allocation took it, and the editor
crashed.  Nothing noticed while lines stayed inside the 64 bytes of slack that
`line_new()` over-allocates -- a line only grows past that when something long
arrives, which is exactly what a paste does (or a long line typed from the character
table).  The helper now takes the BUFFER and the line's INDEX, because there is only
one place that knows where the pointer lives, and the insert loop re-reads the line
every time round.  The test forces the move (a block is allocated right after the
line, so the grow cannot extend in place) and then frees the buffer: the old code
segfaults there, the new one keeps its text through 32 further allocations.

### 6.22 The stutter, the bottom row, a help page that would not close, and one missing letter

**DRAGGING A FILE WITH A LOT OF FOLDS STUTTERED.**  `set_scroll_px()` runs for every
touch move, and it positioned the window by asking "which line is at row N?" -- a
walk over **every line of the file**, asking `nb_fold_hidden()` about each one.  With
2,600 lines and 15 folds that is 39,000 fold comparisons per finger move, plus the
same again in `scroll_px()`; on a 100 MHz calculator that is milliseconds per
event, which is exactly the "strange stutter when there is a lot of folding".  The
lookup is now the inverse of the row counter, in **O(folds) and not O(lines)**, which
needs the fold list sorted -- so `nb_fold_toggle`/`nb_fold_reveal` insert and remove
by shifting instead of swapping the last entry in.  Measured on the host:
`set_scroll_px` 4.95 µs → **0.26 µs**, a drag event 11.5 µs → **0.1 µs** with 15
folds in a 2,600 line file.  The test does not measure time (not reproducible);
it counts `nb_fold_hidden()` calls through `nb_fold_probes` and insists a scroll
costs O(folds), and it checks the list stays sorted after folding *and* unfolding.

**THE ROW AT THE BOTTOM EDGE IS ALWAYS DRAWN.**  The screen is 240 px, the header is
18 and the status bar is 16, so the space between the bars is 206 -- twelve rows and
fourteen pixels.  Drawing twelve rows and letting the next one come and go with the
pixel scroll is why the line above the status bar appeared and disappeared while
dragging.  The renderer now walks **thirteen whole rows** (208 px, down to y = 226)
and the **status bar is painted over the last two pixels** (it is drawn after the
text); the scroll still clamps to 206, so the file's LAST line stays clear of the
bar.  `nb_ui_editor_rows_drawn()` is the invariant the test asks about: every row is
painted at every sub-row offset.

**THE KEY HELP COULD NOT BE CLOSED.**  Its only way out was the HELP key again, and
every key goes through the auto-repeat guard (`if (u->held[code]) return;`), which
clears only when the key's **release** arrives.  One swallowed release therefore left
the help page on the screen for good.  ESC now closes it (and ON, and HELP), the
panel keys (HELP, MENU) are no longer filtered by that bitmap at all -- they toggle
something and have nothing to repeat -- and the test includes the stuck key: a HELP
press with its release dropped still closes the page.

**A CANDIDATE ONE LETTER SHORT.**  The completion box was `longest * 8 + 8` wide
while the text starts 8 px in and is clipped to `w - 12`: the last character of every
candidate was cut, so a function name one letter longer than the typed prefix looked
truncated.  The box is `longest * 8 + 12` now, and the candidate is stored in 40
bytes instead of 24 (which had the same effect for long names).

### 6.23 Search and replace, buttons for the question, and no key legends

**FIND AND REPLACE, ON THE TWO KEYS THAT WERE FREE.**  `SYMB` asks for the text to
search for, `PLOT` for what to put in its place, and the header's right hand end
carries `k/n` -- which match of how many -- with two buttons, `上` and `下`, that
step through the matches (and the count, drawn next to them, is what the user asked
for in the top right corner).  The matching is `src/nb_find.c`: plain text, ASCII
case insensitive, never spanning a line break, no overlapping matches, wrapping at
the ends of the file, and the count stops at 999 (nobody reads `48213/99876`).  The
menu's 全部替换 replaces every match, one undo record each, because a single press
that rewrites a whole file has to be asked for deliberately; PLOT replaces the match
the caret is on and moves to the next one.

**THE QUESTION IS THREE BUTTONS.**  "Save before closing?" was answered with y / n /
ESC -- letters that this keyboard does not print (they are the alpha layer of two
keypad keys) -- so every question is now a row of BUTTONS: 保存 / 不保存 / 取消 for
the save question, 是 / 否 for the others.  LEFT and RIGHT move the highlight, ENTER
or a tap presses it, ESC cancels, and the y / n keys still work silently for the
users who have them in their fingers.  The delete question cannot be answered by
accident with an arrow key any more, which was the point.

**DEL TAKES A WHOLE INDENT STEP.**  Four spaces in, a tab width of four: one press,
four spaces -- and six spaces in goes back to the previous tab stop (two), which is
what makes it predictable when the indentation is not a multiple of the width.  It is
ONE undo record (`nb_buf_backspace_n`), so undoing the press does not put the spaces
back one at a time.  After a word it is the ordinary one character.

**INDENTATION GUIDES.**  A hairline at every indentation level of every line, drawn
under the text in the divider colour; `"indent_guides": false` in setting.json turns
them off (launch file flag bit 4, like the other preferences).

**AND THE SMALL ONES:** the status row no longer spells out a key legend ("↵打开
退格 删除 esc 上级", which was clipped to `esc>` anyway); the file size column stops
one pixel clear of the scrollbar; **every scrollbar is flush with the screen's right
edge** (it was one pixel in); and the row after the last full one is drawn as soon as
the view is mid-row, so there is no blank band above the status bar ("scroll half a
line and the line below the last one did not appear").  **No string the user can read
names an F-key any more** -- the help page's corner, a toast, the key tables -- and
`make test` greps for that shape now (`test-strings`), because the calculator has no
F1..F6 and a legend for a key that does not exist is worse than no legend.

### 6.24 Highlighted results, a floating search window, and two small ones

**EVERY MATCH IS HIGHLIGHTED.**  A search tints every match in the text with a warm
`find` colour (light `#FFF6D8`, dark `#2A2410`) -- under the selection, so a selected
match still reads as selected, and drawn from `nb_find_line()`, which is the same
matcher the count comes from: the panel and the highlight cannot disagree.  The new
colour is held to the theme's rule like every other surface (all nine code classes at
least 4.5:1 on it, which the light tint only just passes at 4.56:1 -- a warmer yellow
failed).

**THE SEARCH CHROME IS A FLOATING WINDOW.**  Two labels squeezed into the title bar
next to the hamburger were ugly, so the count, the two buttons and a close box live in
a small panel drawn over the text at the top right while a search is active:

```
+-------------+
| 3/17      x |
| [ 上 ] [ 下 ]|
+-------------+
```

It is **not** a modal overlay: only the taps inside it belong to it, and a tap outside
is still a tap on the code -- a user stepping through matches is also reading the line
they landed on.

**THE INDENTATION GUIDES ARE COUNTED RIGHT.**  A line with n levels of indentation drew
n guides, and the last one was drawn through the first character of its text.  A guide
marks the BLOCK a level belongs to, so the guides stop one level short of the line's
own indentation: one level has no guide at all (there is nothing to line up with) and
three levels draw two, at columns 4 and 8.

**AND THE TWO SMALL ONES:** the keyboard layer (`alock`, `shift`, `Alpha`) is now
shown BEFORE the file type in the status row, because that is the thing that changes
under the user's fingers; and **a selection drag follows the finger sideways too**.
The edge auto-scroll only knew about the top and bottom bands, so a selection could
never be longer than one line: a finger that left the glass to the left or the right
now scrolls horizontally by one 8 px column per frame, exactly like the vertical step.

### 6.25 The crash after 6.24: the stack the image runs on

**THE IMAGE DOES NOT HAVE A STACK OF ITS OWN.**  It runs INLINE, inside the Python
application's call into the debug device -- so the editor is drawing and editing on
the *Python thread's* stack.  primeLua allocates a stack of its own for exactly this
reason (`plua_run.c`: "the caller's stack, which still runs Lua, just with little
headroom"), and nbCode has always ridden on the caller's.

That is invisible to the emulator, which gives the emulated image as much stack as it
likes, and it is why a release that passed 551 host checks and 129 emulator checks
could reboot the calculator on the first frame: 6.24 added the per-line search
highlight, whose two 24-entry span arrays were declared in `nb_ui_editor_draw` --
**+120 bytes in the frame of the busiest function in the image**.

WHAT CHANGED: every big automatic buffer in the drawing and editing paths is static
scratch now (single-threaded, used and finished inside one call, exactly like the
text scratch the renderer already used):

| frame | before 6.24 | 6.24 (crashed) | now |
|---|---|---|---|
| `nb_ui_editor_draw` | 264 B | 384 B | **264 B** |
| `nb_ui_editor_insert_text` | 632 B | 632 B | **56 B** |
| `nb_ui_editor_copy` | 248 B | 248 B | **56 B** |
| `info_draw` | 576 B | 576 B | **< 32 B** |

...which leaves the image roughly **1.3 KB shallower** than the build that worked, and
the search highlight is kept (`find_span_off`/`find_span_len`).  The sizes are macros
now, because `sizeof` on the pointer that names a static buffer is 4 -- a trap this
change walked into once already.

**AND THE LOG NOW CARRIES THE STACK POINTER.**  `nb_run_begin` and the first frame
write `milestone: start-up sp=0x…` and `milestone: after the first frame sp=0x…` to
`nbcode.log` (`nb_app.c`, device builds only).  The distance between them is what one
frame costs; the distance from them to the crash is what the next report will have.

### 6.26 The file list that showed thirteen entries, and a hex view for what is not text

**THE FINDER'S MASKS ARE NOT FOUR VIEWS OF ONE LIST.**  `list_dir_fw` stopped at
the first attribute mask that returned anything -- "one pass is usually enough",
because three more passes of a folder the first pass had already listed are three
times the calls for the same names.  That was wrong, and the report was exact: *the
file list only shows 13 entries*, with the count in the header agreeing, because
both were short.  The masks are not equivalent: `0x37` sees the ordinary entries and
the long names, the hidden ones and the rest come back under the others, so a folder
of twenty files listed thirteen of them and no amount of scrolling could reach the
rest.

The loop is primeLua's now, argument for argument (`primeLua/calc/main.py` lists a
folder the same way): **all four masks, every time, merged by `merge_find_rows`** --
which is what made one pass look right in the first place, since it dedupes.  The
cost is real (up to four listings per folder) and it is logged per folder
(`[list] … N entr(ies) from M finder result(s) over 4 mask(s)`), and `test_emu`
checks the SHAPE of the loop: an `if out: break` in it is the bug coming back.

**A FILE THAT IS NOT TEXT OPENS IN THE HEX VIEW.**  Latin-1 is the codec's
"everything else" (neither valid UTF-8 nor valid GB2312), so it is what a binary, a
compressed blob or a file in some third encoding comes out as -- and drawing that as
mojibake tells the user nothing.  `src/nb_hex.c` is a read-only view of the first
32 KB: offset, eight bytes in hex, and the printable bytes beside them, in exactly
40 columns:

```
000000  00 01 02 ff fe 68 65 6c 6c 6f 7f     .....hello.
```

The keys are the file list's (arrows, page, `shift`+`←`/`→` for home and end, `esc`
back to the list), dragging moves the bytes with the finger, and the scrollbar is the
same one.  The status row says which bytes are on the glass and how big the file is
(`（只读前 32K）` when it is bigger than the view).  **Its MENU hands the file to the
editor** ("以文本打开"): a Latin-1 source file is a real thing, and the guess that
sent it to the hex view was a guess.  Editing bytes is deliberately NOT here -- look
at the bytes and hand the file to the editor, one decision at a time.

---

## 7. Repository layout

```
Makefile                the build, deploy, dist and test targets
README.md               this file (deployed as nbCode.hpappdir\nbCode.md)
setting.json            the settings the user edits
samples/                files deployed for the first run (hello.c, hello.lua,
                        notes.txt)

src/                    the portable editor: NOTHING here includes a firmware
                        header, which is why the host tests compile it unmodified
  nb_buffer.c           lines, cursor, undo (inverse-operation stack)
  nb_codec.c            UTF-8 <-> GB2312, so Chinese text round-trips
  nb_font.c             glyph lookup over the generated Unifont face
  nb_highlight.c        the C / Lua / Python / PPL / text tokenizer
  nb_text.c             text measurement and drawing
  nb_gfx.c              framebuffer primitives
  nb_ui*.c              the screens: start, browser, editor, prompts, help
  nb_clip.c             the global clipboard: ten entries + the file
  nb_find.c             search and replace (the matching only)
  nb_hex.c              the hex view: what a non-text file opens in
  nb_keymap.c           device key codes -> editor keys
  nb_list.c             the workspace and file list the launcher handed over
  nb_app.c              the run: config, install the hook, frame loop, exit
  nb_cfg.h              THE LAUNCH FILE FORMAT (Python and C both read this)

port/                   everything that touches the calculator
  nb_svc.s              nb_entry + the firmware SVC stubs + the input hook
  nb_fw.c               heap, string core, clock, log ring, log file
  nb_cfgfile.c          the defensive parser for nbcode.cfg
  nb_file.c             UTF-16 paths and the firmware's "rb"/"wb+" modes
  nb_screen.c           the real ARGB8888 LCD
  nb_input_fw.c         the get_event slot patch (CP15 cache ritual)
  nb_link.ld            the layout the loader expects
  nbcode_font_data.c/h   the generated Unifont face (8,518 glyphs)

host/                   the PC side of the port layer, for the tests
calc/main.py            the MicroPython launcher (deployed STRIPPED: the
                        calculator compiles a module's source into its own RAM)
calc/main_notes.md      the launcher's longer notes (deployed with it)
tests/test_host.c      571 checks: buffer, codec, highlighter, keymap,
                        UI flow, the pixel viewport, the touch panels, the
                        launch file's directory table, the palette's numbers,
                        the layer buffer
tests/test_emu.py       132 checks: the real image, loaded and driven
tests/emu_arm2.py       the vendored ARMv5 interpreter
samples/                three tiny files the deploy puts next to the editor
tools/gen_font.py       Unifont .hex -> the compiled face
tools/fix_relocs.py     folds symbol relocations into R_ARM_RELATIVE
tools/strip_prose.py    main.py without its comments, for the deployed copy
tools/make_dist.py      validates and packages the release
```

---

## 8. Tests

`make test-host` compiles the editor's portable half on the PC against a
simulated screen and a scripted keyboard: 571 checks over the buffer (insert,
delete, join, split, undo), the codec (UTF-8 and GB2312 round-trips), the
highlighter (all five languages, multi-line state), the **keyboard** (all 32
printed letters, the ALPHA one-shot/lock cycle, and that the three layers reach
every printable ASCII byte), the **viewport** (a drag in pixels on both axes, the
clamps at either end, the caret follow, and a selection measured in the
framebuffer so a full-width character is highlighted whole), the **panels**
(the character table and its glyph/encoding rules, the hamburger menu's four
actions, the header drawn last, and the scrollbar), the **layer buffer**
(drawing does not reach the panel until it is presented) and the selection's edge
scroll, rendering (glyph metrics,
clipping, scroll) and the UI flow (open a file, type, save, quit, unsaved-change
prompts).

`make test-emu` is the interesting one: it loads **the real nbCode.elf** into an
ARMv5 interpreter the way the calculator does -- `malloc`, copy the segments,
apply the relocations -- runs it from `nb_entry`, and asserts on what happened:

* the image is well formed (all relocations `R_ARM_RELATIVE`, the `NBCORING` ring
  and the `NBINPUT` input state are inside the loaded segments);
* the launcher (`calc/main.py`, the real file) can write a `nbcode.cfg` the C
  parser accepts -- the drift between the two sides that cost three rewrites is
  now a failing test instead of a black screen;
* the editor opens the file the launcher named, runs its frame loop, and exits
  cleanly with a status published as `RT_RET:`;
* a key event injected into the emulated firmware queue goes through the patched
  slot, into the image's hook, out of its queue and into the editor -- the whole
  chain, including the CP15 patch;
* the launcher's stand-down restores the firmware's `get_event` bytes word for
  word;
* `main.probe()` -- the recovery path §6 tells a user to run first -- walks the
  whole chain against the real image and reports the entry point, the ring
  address and the load base;
* the workspace list from `setting.json` normalises the way the README says
  (bare paths, `[label, path]` pairs, and garbage falling back to the defaults);
* the six device constraints of §6.1 hold -- no `struct`, no `time`, no `json`,
  no `os`, no `__name__` entry point, no absolute paths -- including a test that
  the **stripped** launcher `make deploy` ships still builds the same launch file
  and parses the same settings as the commented source it came from;
* the load stays inside a round-trip budget (§6.2) and uses the firmware's
  `calloc` and `fread` rather than word at a time writes;
* the editor really drew into the **firmware's** framebuffer and left the LCD
  info structure alone (§6.3) -- the bug that rebooted the calculator on its
  first run, and that the emulator's layout had been hiding;
* the image left its startup milestones in the durable log, so a fault that never
  returns is still diagnosable;
* **the codec is the device's, not CPython's** (§6.10): the launcher runs with a
  `bytes` whose decoder raises on a non-strict error handler, a ring window that
  opens inside a Chinese character is handed to both ring readers, and an AST
  check refuses an `errors=` argument in the deployed source;
* **a normal session says nothing** (§6.11): the console output of a whole run is
  captured and must be empty, while the durable log still gets every line, and the
  `verbose` marker really does bring the printing back.

---

## 9. Where this comes from

nbCode is the fourth project in this family, and it borrows deliberately:

* **DOOM for the HP Prime** -- the shellcode loader and the input hook mechanism.
* **primetcc** -- the firmware service table, the debug-channel protocol, and the
  ARMv5 emulator the tests run on.
* **primeLua** -- the launcher's shape, the file API quirks (`fread`/`fwrite`
  return *items*, not bytes; `fopen` wants UTF-16 paths and only reliably
  supports `"rb"`/`"wb+"`), the `RT_RET:` exit status, and the CP15 patch ritual.
* **Python PrimeCode** -- the file manager idiom the start screen copies.

The difference is what it is for: an editor has to redraw a screenful of text
every frame and hold an undo stack, which is why it is C rather than Python.

The face is **GNU Unifont** (GPL + font exception), compiled in by
`tools/gen_font.py`.
