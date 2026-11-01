# nbCode launcher notes

`calc/main.py` carries the prose in its own docstrings and comments; this file is
where the launcher's longer explanations live, and `make deploy` copies it next to
the code so the reasoning travels with the deployment.

## Why the launcher exists at all

The calculator's MicroPython application is the only place in this system with:

* a directory enumeration (`os.listdir` / the firmware's `W_FIND_FIRST`), which a
  shellcode-loaded image does NOT have -- the firmware's own file API can open a
  path but cannot walk a folder;
* a JSON parser, for `setting.json`;
* a way to start native code at all (the debug device and the shellcode loader).

So the launcher answers "which files are there" and "what does the user want
nbCode configured to do", writes one flat **launch file** next to the image
(`nbcode.cfg`, format in src/nb_cfg.h), and then gets out of the way: the editor
owns the screen and the keyboard until the user quits.

The handoff is a file rather than a struct in memory because a memory block has a
layout the two sides can disagree about, and they did -- three times, each one
ending in a black screen and a "no config block" line in the log.  A file has no
layout: a short file, a wrong magic or a missing file all end in "start with the
defaults and say so".  `calc/main.py`'s `CFG_*` constants are checked against
`src/nb_cfg.h` by `tools/make_dist.py`, so the drift is caught at package time.

## Why the editor is C

The Python PrimeCode in this workspace works, and its file-manager idiom is what
nbCode's start screen copies.  What it cannot do is draw a 320x240 screen of 8x16
bitmap glyphs thirty times a second: MicroPython on this CPU spends on the order
of 100 ms redrawing a screenful of text.  The C side does the same screen in a
couple of milliseconds, which is what makes a 40x13 editor with syntax colouring
usable at all.

## The debug channel

`PrimeDebug` speaks the protocol the firmware's `"debug"` device implements
(REVERSE_README.md): write `{op, addr, value}` triples and read the answers back.
`op 0` reads memory, `op 1` writes a word, `op 2` calls an address with up to four
arguments.  This is the same interface DOOM, primetcc and primeLua drive their
images through.

## The console is quiet (and the log file is not)

`log()` appends to `nbcode.log` -- the durable file, which is where a crash gets
diagnosed from and which survives a reboot -- and PRINTS nothing.  A normal
start-up and exit used to put every `[load]`/`[cost]`/`[hook]` line in front of
the user, and since the editor runs inside ONE debug call, start-up and exit is all
the output there ever was.  `alert()` is the version that speaks: a failed load, a
refused `fopen`, invalid JSON in `setting.json`, an editor that did not return.
Put an empty file named `verbose` next to `main.py` to get the whole stream back
(the same idiom as primeLua's `nothread` marker), and note that `main.probe()`
prints regardless: it exists to be read.

## What the console SAYS, and who decides how it looks

Every printed line goes through a **level**, and the levels are `moreprint`'s --
the module primeLua and primetcc import, which the user drops next to `main.py`:

| helper | for | who calls it |
|---|---|---|
| `p_log` | what the launcher is doing | `log()` (the default level), `progress()` |
| `p_warning` | a fallback worth a look | `note()` -- malloc instead of calloc, an `fopen` spelling retried, `setting.json` that is not JSON |
| `p_error` | something failed | `alert()` -- every caller is a failure path |
| `p_pass` | a step that worked | `succeed()` -- the line that says the editor came back cleanly |

`p_out` (a program's own stdout) is the one nbCode does not take: it prints no
program output at all, because the editor owns the screen from the moment it starts.
All four fall back to `print()`, so a calculator without `moreprint.py` reads exactly
as it did before and only loses the colour.  A call site never chooses a colour: it
chooses a LEVEL, and the module decides what that looks like.

## The clipboard is a file next to the log

`shift`+VIEW copies (the selection, or the caret's line) into a **global** clipboard:
ten entries, shared by every file the editor opens, and written to `nbclip.txt` in
the application folder so the next session still has them (`shift`+MENU opens the
panel; a tap inserts, `DEL` deletes).  The C side owns it (`src/nb_clip.c`) and the
launcher does nothing for it -- but the PATH is the launcher's business: the only
path the launcher tells the editor is `logpath` in the launch file, so the clipboard
is that path with the file name replaced (`nb_app.c`).  No launch file format change,
no version bump.  `clip entries=N` in the durable log is how a session says how many
entries it recovered.

## Listing a folder: every mask, every time

The firmware's finder is asked once per attribute mask and the answers are merged
(`merge_find_rows`).  The masks are NOT four views of one list -- 0x37 sees the
ordinary entries, the long names and the hidden ones come back under the others --
so stopping at the first mask that returned anything lost entries: "the file list
only shows 13 entries" was a folder of twenty, and the count in the header agreed
with the short list because both came from the same pass.  primeLua's launcher runs
all four masks and merges, and this one is the same loop now.  The cost is logged per
folder, and `test_emu` fails if an `if out: break` comes back into that loop.

## The exit status

The debug interface's return value is not trustworthy on this hardware (primetcc
measured it reporting 2 whatever the callee returns), so the image writes
`RT_RET:<n>` into its log ring on the way out and the launcher parses that.  The
same trick is nbCode's console.  The run is ONE debug call: it does not return
until the editor exits, and the ring is read out then.  (primeLua runs its image
in a firmware thread instead, so its launcher can keep polling while a script
runs -- it needs that because a Lua script prints as it goes.  nbCode's editor
owns the screen for the whole session and writes the lines that matter to
nbcode.log itself, so the thread buys nothing and costs a spin loop.)

## What the calculator does not have

Every line here was a failure on hardware before it was a rule in `main.py`, and
every one of them is now a test (`tests/test_emu.py`, the `device:` checks):

* **no `struct`** -- the module is called `ustruct`; `import struct` fails;
* **no `time`** -- not imported at all (nothing here sleeps or measures time);
* **no `json`/`ujson`** -- so `setting.json` is read by the launcher's own
  `parse_json`, and the test requires it to agree with CPython's `json`;
* **no `os` worth using** -- listing a folder, stat-ing a file and finding the
  working directory all go through the FIRMWARE (`W_FIND_FIRST`/`W_FIND_NEXT`,
  `W_GET_ATTR`, `W_GETCWD`), which is the only thing that can see outside the
  application folder;
* **no `__name__ == '__main__'`** -- the file is run by the Python application,
  not by CPython, so the entry point at the bottom of `main.py` keys off the
  interpreter itself.  Nothing runs on import on a host, which is what lets the
  tests load this file as a module;
* **no absolute paths** -- `uio.FileIO("C:\\DATA\\...")` raises `OSError 0`.
  Everything Python opens is a BARE NAME, resolved against the application
  folder.  This is the trap that killed the first hardware run, in a launcher
  whose own docstring had been assuming the opposite for weeks;
* **no non-strict codec error handlers** -- there is no `errors='replace'` and no
  `errors='ignore'`; passing one is a `UnicodeError` there whatever the data is.
  The launcher parses the image's log ring with the last 64 bytes of a stream
  whose messages are Chinese, so a window that opens in the MIDDLE of a character
  is the normal case: that is the `UnicodeError` the user saw on the way out.
  `decode_bytes()` therefore tries UTF-8, then latin-1, then a per-byte
  conversion, and `utf8_bytes()` writes UTF-8 by hand; nothing in this file
  passes `errors=` to anything.  `tests/test_emu.py` runs the whole launcher with
  the device's `bytes` (a non-strict handler raises) and refuses a non-strict
  handler in the deployed source by AST.

The two halves therefore use opposite spellings of one file.  The launcher writes
`nbcode.cfg` by bare name; the image opens it with the firmware's API, which wants
the device path.  The launcher hands that folder over in **r0** at entry -- a
pointer to a NUL-terminated UTF-8 string, NOT a struct (a string has no layout for
the two sides to disagree about, which is the lesson that produced the launch
file in the first place).  The image copies it, and falls back to its build-time
constant and then to the bare name.

## The load, and why it is not minutes long

A debug call is a device write plus a read.  Writing a 482 KB image through that
protocol one word at a time is ~124,000 round trips, which is what the first
hardware attempts were doing.  Two firmware services fix it, and both are used
only when they work:

* **`calloc` (svc 0x10038)** for the image block: the block arrives zeroed, so the
  116 KB of `.bss` is never written at all (and the heap does NOT hand back zeroed
  memory, so this is correctness as well as speed);
* **`fread` (svc 0x100D4)**, which takes the destination pointer: one call copies a
  whole `PT_LOAD` segment straight into the block, and the copy is verified by
  reading the first bytes back before the image is trusted.

The whole session -- load, 60 frames of editor, teardown -- is now **222 round
trips** (197 of them the load, which §6.9 of the README moved into the firmware's
loader), and `tests/test_emu.py` fails if it goes over 8,000.  The word at a time
path is still there, still tested (on a tiny synthetic image), and is what runs if
the firmware refuses either service.

## The LCD is behind a pointer (this cost a reboot)

The first hardware run printed `nbCode 1.0 start` and then the calculator faulted.
`get_lcd()` returns the LCD STRUCTURE; its first field points at the LCD INFO
structure, and the pixels are at `*(u32 *)(info + 16)`:

    p  = get_lcd();
    q  = *(u32 *)p;
    w  = *(u16 *)(q + 2);
    h  = *(u16 *)(q + 4);
    fb = *(u32 *)(q + 16);

Taking `q` as the framebuffer writes every frame into firmware memory, and a range
check does not help -- the wrong pointer is in range.  The emulator hides this: it
puts the info structure 64 bytes before the pixels, so the bug draws a picture 16
pixels off instead of rebooting, which is why `tests/test_emu.py` now checks the
firmware's framebuffer AND that its info structure is untouched.

## Milestones exist because a reboot eats the ring

The run is ONE blocking debug call, so the launcher reads the image's ring only
after the editor returns.  A REBOOT never returns: the ring is lost, and the only
witness left is what the image wrote to the flash itself.  So nb_run_begin()
writes a few short milestones straight into nbcode.log -- config loaded, screen
ready, ui ready, input hook armed, first frame -- which is what turns "it rebooted
after nbCode 1.0 start" into "it died at the first frame".  The config is parsed
BEFORE the screen so that the log path is known in time; a milestone written
before that would go nowhere.

## Two device rules that look like bugs elsewhere

**The glyph origin is the ink box inside the cell.**  `nb_text.c` used to draw
half-width glyphs at `x0 - 8`, on the belief that their ink sat in the high byte
of the 16 bit row word.  It sits in the LOW bits (`'A'` row 0 is `0x000C`) and
`glyph_x0` is already cell relative, so every western character was drawn one
cell to the left of the cursor.  The host tests draw at x=0, where a negative
origin is clipped away, and the cache comparison shared the same wrong rule --
neither could see it.  They measure the ink columns now.

**The image may not be freed while the OS is inside its hook.**  The input thread
blocks inside `nb_input_hook` (in the firmware's `get_event`) whenever there is no
event pending, which is most of the time.  Quitting the editor left that thread
inside the image; the launcher freed the block, and the next event resumed the
thread in freed memory -- a reboot, and the reason "esc, esc" restarted the
calculator.  `nb_in_state.hook_busy` counts the calls in flight, and the launcher
frees the image only when it reads zero (otherwise it holds the block and
releases it before the next load).  While the editor runs the counter is
non-zero, so a *normal* quit holds 649 KB for a moment rather than resetting the
machine.

## The highlight state array is one byte per line, and it must start clean

The highlighter keeps "which block does line N start in" as one byte per line, in
an array grown with `nb_realloc`.  That array is READ by the next line's
tokenizer, so a byte that means "inside a long string" paints everything after it
green -- and `realloc` does not zero, so the fresh bytes used to be heap garbage.
That is why some files opened all green and others did not: it depended on what
the heap had held.  `nb_hl_open()` now keeps the array across files (the old
`nb_hl_init()` threw it away, leaking it, and allocated an uninitialised one in
its place) and `nb_hl_lines()` zeroes what it grows.  The host test poisons the
heap first, so this cannot come back quietly.

## Leaving the editor asks WHERE TO

ESC and F1 mean "back to the file list", ON means "out of the program", and both ask
about unsaved changes.  One "save before quit?" question served both, so ESC + 'n'
quit the editor instead of closing the file.  The question now carries its
destination, and ESC on the question itself cancels (it does not count as "no"): in
that state "no" and "never mind" are different answers, and only one of them should
lose the user's work.

## What an edit invalidates

Three things, and each one was a bug:

* a line's style buffer holds one byte per byte of the line, so a line that GREW
  needs a bigger buffer.  Allocating only when the buffer was missing wrote one byte
  past the end of it -- into the next heap allocation, usually the following line's
  buffer.  "Insert a character into a .txt and the colours below shift" is that byte;
* a newline renumbers every line below it, so the recorded per-line BLOCK STATES are
  stale from there down (and the style buffers belong to different lines now);
* `stop_when_stable` ("the later lines are still coloured correctly") is only true if
  they were coloured at all.  The draw path colours every visible line now, so a
  screenful inside one long comment is not half plain.

## The launch file carries one file list PER WORKSPACE

The start screen offers several folders, and the browser can only show what the
launcher listed.  The format's first shape carried one list (the default
workspace's), which made every choice show nbCode's own files; it now carries one
list per workspace (format v3, `src/nb_cfg.h`), with the names in a shared pool so
the image's static memory does not grow by megabytes.  When a workspace's files are
evicted because the pool or the entry cap is full, the parser says so in the log
rather than silently dropping them.

## The palette is chosen by measured contrast

A 1-pixel-stroke bitmap font on a 320x240 LCD has no antialiasing to help a
mid-tone colour be readable, so the syntax colours are picked by contrast ratio
against their own background: code classes at WCAG AAA (7:1), comments and the
line-number gutter at 4.5:1, and the same for the status bar, the header and the
selected row.  The host tests compute the ratios from the built theme, so a
"prettier" colour that cannot be read fails the suite instead of shipping.

## Folders: the launcher lists them, the image browses them

The image has no directory API, so a folder it can show is a folder the launcher
listed.  The launch file therefore carries a table of directories (format v4), each
with a parent and its own entries; a folder row in the list carries the index of the
directory it points at, which is what makes `ENTER` on `.hpappdir` work without
another round trip to Python.  The launcher lists two levels (a workspace and the
folders inside it) within a budget it logs, and `ESC` goes up one folder before it
leaves for the workspace list.

THE WALK IS THE TABLE.  Every directory that is found is appended to `dirs`, so the
table *is* the queue: walking `i` forward over it *is* the breadth-first order, and
`i` -- the entry being expanded -- is exactly the parent index its children need.
The queue this replaced held `(dict, depth)` pairs and looked the parent up again
with `dirs.index(parent)`: a search over the whole table comparing dictionaries **by
value**, which answers the first directory that happens to be equal.  It is also
where a user's crash report pointed (`'…' object has no attribute append` on the push
line), so the line, the queue and the search are all gone rather than guessed at.

A folder deeper than `CFG_DIR_DEPTH` is a row that cannot be opened, and the walk
CONTINUES rather than ending.  And the whole phase is wrapped in a `try`: **a listing
that fails may not stop the editor.**  It logs the exception and the types involved
(`dirs` and `workspaces` are the two things the walk assumes about its inputs) and
carries on with the table built so far, because the editor is what the user asked for
and this phase only fills in the folders behind its browser.

## The image stays mapped between runs

Loading is the expensive phase of a start-up: 163 of 189 debug round trips in the
emulator, and a hundred thousand if the firmware will not do the copying.  A
finished session therefore KEEPS the image mapped and the next run re-enters it --
26 round trips for the whole start-up, 1 for the load.  That is also the safe
choice: the OS's input thread can be inside the hook when the session ends, and the
block must not be freed under it.  `main.unload()` gives the memory back, and a
different image releases it before the new one is loaded.

`[cost] ...` lines in `pcode.log` say where a slow start-up went: the phases are
counted in debug round trips, because there is no clock on this machine.

## The image is loaded BY THE FIRMWARE, not by Python

Copying an image from Python costs one debug round trip per four bytes: 121,875 of
them for nbCode, measured in this launcher's own log.  So the launcher uploads
primeLua's loader blob (690 bytes of ARM code) into the firmware heap and calls it
with (path, image_base): the blob does fopen, the segment copies and the
relocations in ARM code and returns the entry.  Python verifies the result and must
NOT relocate again.  The blob is uploaded once per session; the first start-up
costs about 197 round trips and a repeat costs 1, because the image stays mapped.

## Touch

Drag scrolls by pixels (a sub-line `yoff`, with one extra row drawn), a long press
(12 frames) starts a selection that a further drag extends, and a tap puts the
caret.  F5 cuts the selection, typing replaces it, and a cut too big for one undo
record says so rather than undoing only its first 96 bytes.

## What to check first on a real calculator

1. `main.probe()` -- walks the load chain (a bare name reads, the settings, the
   workspace list, the folder listing, the ELF header, the allocation, the
   relocations, the ring magic) and logs each step BEFORE it happens, so the last
   line of `nbcode.log` names the culprit.
2. `nbcode.log` -- written by the image itself through the firmware's file API
   (nb_fw.c: nb_logfile), so it survives a reset that the ring does not.
3. The input hook -- `main.stand_down_input()` restores the firmware's
   `get_event` slot from the bytes the image saved before patching it.  If the
   keyboard is dead after a crash, that function (and the launcher's exit path
   that calls it) is what should have fixed it.
