# nbCode -- a C code editor for the HP Prime G1, built the way primeLua is.
#
# WHAT THIS BUILDS
#   build/nbCode.hpappdir/nbCode.elf   the editor: freestanding ARMv5TE,
#                                    position independent, entry nb_entry --
#                                    the shape the calculator's MicroPython
#                                    shellcode loader (DOOM / primetcc /
#                                    primeLua) can load and run.
#   ../nbCode.hpappdir/               the flat deployment folder for C:\DATA\
#
# TOOLCHAIN: arm-none-eabi-gcc (Ubuntu package gcc-arm-none-eabi).
#
# THE ONE DEPENDENCY ON A SIBLING PROJECT is the shellcode loader itself, which
# lives on the calculator (calc/main.py) and in primeLua's tests.  Nothing from
# primetcc or primeLua is compiled in: nbCode carries its own heap wrapper, string
# core and font, so a source checkout of this directory builds without them.

TOPDIR   := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD    := $(TOPDIR)/build
OBJ      := $(BUILD)/obj
APPNAME  := nbCode.hpappdir
DEPLOY   := $(TOPDIR)/../$(APPNAME)

CC       := arm-none-eabi-gcc
CC_HOST  := cc
AS       := arm-none-eabi-as
OBJCOPY  := arm-none-eabi-objcopy
READELF  := arm-none-eabi-readelf
SIZE     := arm-none-eabi-size
PYTHON   := python3

ARCH     := -mcpu=arm926ej-s -marm -mfloat-abi=soft

# -ffreestanding  no libc headers, and no assumption that main() is the entry
# -fPIC           the loader applies R_ARM_RELATIVE relocations only
# -fno-builtin    the string core is nbCode's own; without this GCC turns
#                 nb_memcpy's loop into a call to memcpy, which is itself
# -fno-strict-aliasing  the input hook reads a firmware struct by offset
# -ffunction-sections/-fdata-sections + --gc-sections: the font is 360 KB of
#                 table and only the glyph lookups are reachable, so anything
#                 the editor does not use must be droppable
CFLAGS   := $(ARCH) -ffreestanding -fno-builtin -fPIC -ffunction-sections \
            -fdata-sections -O2 -fno-strict-aliasing -Wall -Wno-unused-parameter \
            -I$(TOPDIR)/src -I$(TOPDIR)/port

# the font is generated (tools/gen_font.py) and is the only generated source
FONT_C   := $(TOPDIR)/port/nbcode_font_data.c
FONT_H   := $(TOPDIR)/port/nbcode_font_data.h

# the portable half: no firmware header, no malloc -- these are the files the
# host tests compile too, which is what makes the tests evidence about the real
# editor instead of about a model of it
CORE_SRCS := $(TOPDIR)/src/nb_buffer.c $(TOPDIR)/src/nb_codec.c \
             $(TOPDIR)/src/nb_font.c $(TOPDIR)/src/nb_highlight.c \
             $(TOPDIR)/src/nb_text.c $(TOPDIR)/src/nb_gfx.c \
             $(TOPDIR)/src/nb_util.c $(TOPDIR)/src/nb_ui.c \
             $(TOPDIR)/src/nb_ui_draw.c $(TOPDIR)/src/nb_ui_browser.c \
             $(TOPDIR)/src/nb_ui_editor.c $(TOPDIR)/src/nb_ui_prompt.c \
             $(TOPDIR)/src/nb_ui_overlay.c $(TOPDIR)/src/nb_complete.c \
             $(TOPDIR)/src/nb_fold.c $(TOPDIR)/src/nb_clip.c \
             $(TOPDIR)/src/nb_find.c $(TOPDIR)/src/nb_hex.c \
             $(TOPDIR)/src/nb_keymap.c $(TOPDIR)/src/nb_app.c \
             $(TOPDIR)/src/nb_list.c

PORT_SRCS := $(TOPDIR)/port/nb_fw.c $(TOPDIR)/port/nb_cfgfile.c \
             $(TOPDIR)/port/nb_file.c $(TOPDIR)/port/nb_screen.c \
             $(TOPDIR)/port/nb_input_fw.c $(FONT_C)

CORE_OBJS := $(patsubst $(TOPDIR)/src/%.c,$(OBJ)/src/%.o,$(CORE_SRCS))
PORT_OBJS := $(patsubst $(TOPDIR)/port/%.c,$(OBJ)/port/%.o,$(PORT_SRCS)) \
             $(OBJ)/port/nb_svc.o

ELF      := $(BUILD)/$(APPNAME)/nbCode.elf
HOSTBIN  := $(BUILD)/host/nb_test

HEADERS  := $(wildcard $(TOPDIR)/src/*.h) $(wildcard $(TOPDIR)/port/*.h)

# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------
all: $(ELF) deploy

$(ELF): $(CORE_OBJS) $(PORT_OBJS) | $(BUILD)/$(APPNAME)
	$(CC) -nostdlib -fPIC -pie -Wl,-e,nb_entry -Wl,--gc-sections \
	    -Wl,-T,$(TOPDIR)/port/nb_link.ld \
	    $(CORE_OBJS) $(PORT_OBJS) -lgcc -o $@
	@$(PYTHON) $(TOPDIR)/tools/fix_relocs.py $@
	@echo "==> $@ built"
	@$(READELF) -h $@ | grep -E 'Type|Entry|Machine'
	@$(READELF) -r $@ | awk 'NF>2 {print $$3}' | sort | uniq -c | sort -rn | head -5
	@$(SIZE) $@

$(OBJ)/src/%.o: $(TOPDIR)/src/%.c $(HEADERS) $(FONT_H) | $(OBJ)/src
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/port/%.o: $(TOPDIR)/port/%.c $(HEADERS) $(FONT_H) | $(OBJ)/port
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/port/nb_svc.o: $(TOPDIR)/port/nb_svc.s | $(OBJ)/port
	$(AS) -mcpu=arm926ej-s $< -o $@

$(OBJ)/src $(OBJ)/port $(BUILD)/$(APPNAME) $(BUILD)/host:
	@mkdir -p $@

# ---------------------------------------------------------------------------
# the font: regenerated from GNU Unifont, then compiled into the image
# ---------------------------------------------------------------------------
font:
	@$(PYTHON) $(TOPDIR)/tools/gen_font.py
	@echo "==> now rebuild: the face is compiled into nbCode.elf"

$(FONT_C) $(FONT_H): $(TOPDIR)/tools/gen_font.py
	@$(PYTHON) $(TOPDIR)/tools/gen_font.py

# ---------------------------------------------------------------------------
# deployment: a flat folder dropped into the calculator's C:\DATA\
#
# THE DEPLOY FOLDER IS A BUILD PRODUCT: wiped and rebuilt, so a file this
# version no longer ships cannot survive in it.
# ---------------------------------------------------------------------------
deploy: $(ELF)
	@rm -rf $(DEPLOY)
	@mkdir -p $(DEPLOY)
# The image the loader copies: nbCode's device build carries no -g, so this
# mostly drops the symbol table (14 KB of debug-channel traffic).  What is left
# of the file is the ELF header, the program headers and the two PT_LOAD
# segments -- which is all the shellcode loader reads.
	@$(OBJCOPY) --strip-debug $(ELF) $(DEPLOY)/nbCode.elf
# the launcher, its notes, and the settings file the user can edit
#
# main.py is DEPLOYED STRIPPED: the calculator compiles an imported module's
# whole source into its own memory (primeLua measured ~6 MB of heap for 156 KB of
# source), and 42% of this file is comments and docstrings.  tools/strip_prose.py
# removes them for the copy that ships and refuses to write anything that does
# not parse; calc/main.py keeps every word.
	@$(PYTHON) $(TOPDIR)/tools/strip_prose.py $(TOPDIR)/calc/main.py $(DEPLOY)/main.py
	@cp $(TOPDIR)/calc/main_notes.md $(DEPLOY)/
	@cp $(TOPDIR)/setting.json $(DEPLOY)/setting.json
	@cp $(TOPDIR)/README.md $(DEPLOY)/nbCode.md
# three tiny sample files, so the first run has something to open (and so the
# start screen is not an empty list on a fresh calculator)
	@cp $(TOPDIR)/samples/* $(DEPLOY)/
	@echo "==> deployed to $(DEPLOY)"
	@ls -la $(DEPLOY)

DIST := $(TOPDIR)/dist

dist: deploy
	@$(PYTHON) $(TOPDIR)/tools/make_dist.py
	@ls -la $(DIST)

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
# The launch file's PARSER is compiled in on the host too: it is the same
# nbcode.cfg the launcher writes for the calculator, so the format is tested here
# before it is tested on the device.
$(HOSTBIN): $(TOPDIR)/tests/test_host.c $(CORE_SRCS) $(TOPDIR)/host/nb_port_host.c \
            $(TOPDIR)/host/nb_host_ext.c $(TOPDIR)/port/nb_cfgfile.c $(FONT_C) \
            $(HEADERS) | $(BUILD)/host
	$(CC_HOST) -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter \
	    -I$(TOPDIR)/src -I$(TOPDIR)/port -I$(TOPDIR)/host \
	    -o $@ $(TOPDIR)/tests/test_host.c $(CORE_SRCS) $(FONT_C) \
	    $(TOPDIR)/port/nb_cfgfile.c \
	    $(TOPDIR)/host/nb_port_host.c $(TOPDIR)/host/nb_host_ext.c -lm

# the core, on the PC: buffer, codec, highlighter, renderer, UI flow
test-host: $(HOSTBIN)
	@$(HOSTBIN)

# the same editor, loaded the way the calculator loads it, driven with key
# events through the firmware's own input slot
# Run in place: the suite needs its own directory (tests/emu_arm2.py, the
# vendored emulator, and tests/emu_fixes.py sit next to it), so a copy under
# build/ would have to drag both along anyway.
test-emu: $(ELF)
	@cd $(TOPDIR) && $(PYTHON) tests/test_emu.py

# THE CALCULATOR HAS NO F1..F6 (nb_ui.h): no STRING the user can read may name
# one.  The rule is here and not in a host test because the sources are what it
# is about: `"F1 "` inside a string literal is a legend for a key that does not
# exist, and this greps for exactly that shape.
test-strings:
	@if grep -nE '"[^"]*\bF[1-6]\b' $(CORE_SRCS) $(PORT_SRCS); then \
		echo "FAIL: an F-key name is in a user-visible string"; exit 1; \
	else echo "ok   strings: no F-key name in any user-visible string"; fi

test: test-strings test-host test-emu
	@echo "==> all nbCode tests passed"

clean:
	@rm -rf $(BUILD)

.PHONY: all deploy dist test test-host test-emu font clean
