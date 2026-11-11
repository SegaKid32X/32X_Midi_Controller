# ======================================================================
# 32X MIDI Controller - a GenMDM front end for the Sega 32X
#                       (+ Mega EverDrive PRO + the GenMDM interface)
#
#   make            rom/midi32x.32x          the program (patches from the SD card)
#   make test-rom   rom/midi32x-test.32x     with test patches built in (emulator)
#   make md         rom/midimd.bin           the Genesis / Mega Drive alone (no 32X)
#   make md-test    rom/midimd-test.bin      the same with test patches built in
#   make check      ROM checks + host tests (+ PicoDrive if tests/emu has it)
#   make clean
#
# Toolchain: sh-elf and m68k-elf GCC in $(GENDEV) (Chilly Willy's 32XDK
# layout; built here from Ubuntu's GCC 12 sources as for SegaTube).
# ======================================================================

GENDEV ?= /opt/toolchains/sega
TARGET  := midi32x
OBJ     := obj
ROM     := rom/$(TARGET).32x
TESTROM := rom/$(TARGET)-test.32x

SHPREFIX := $(GENDEV)/sh-elf/bin/sh-elf-
MDPREFIX := $(GENDEV)/m68k-elf/bin/m68k-elf-
CC     := $(SHPREFIX)gcc
AS     := $(SHPREFIX)as
OBJC   := $(SHPREFIX)objcopy
SIZE   := $(SHPREFIX)size
MDAS   := $(MDPREFIX)as
MDCC   := $(MDPREFIX)gcc
MDOBJC := $(MDPREFIX)objcopy
HOSTCC ?= cc

MDCFLAGS = -m68000 -O2 -fomit-frame-pointer -ffreestanding -nostdlib -Wall -Wextra \
           -Wno-unused-parameter -Isrc/common -Isrc/include -I$(OBJ) $(EXTRA_CFLAGS)
SHFLAGS := -c -std=gnu11 -m2 -mb -O2 -fomit-frame-pointer -ffreestanding \
           -ffunction-sections -fdata-sections -fno-common \
           -Wall -Wextra -Wno-unused-parameter -D__32X__ \
           -Isrc/32x -Isrc/common -Isrc/include -I$(OBJ) $(EXTRA_CFLAGS)
LDFLAGS := -T src/32x/mars.ld -nostdlib -Wl,--gc-sections -Wl,-Map=$(OBJ)/$(TARGET).map
LIBS    := -lgcc

SH_SRC  := $(wildcard src/32x/*.c) src/common/patch.c
SH_OBJS := $(OBJ)/crt0.o $(patsubst %.c,$(OBJ)/sh/%.o,$(notdir $(SH_SRC)))
MD_OBJS := $(OBJ)/md_crt.o $(patsubst src/md/%.c,$(OBJ)/md/%.o,$(wildcard src/md/*.c))
HEADERS := $(wildcard src/32x/*.h src/common/*.h src/include/*.h)

MARKERS := --title "32X MIDI CONTROLLER" --marker "32X MIDI CONTROLLER"

.PHONY: all test-rom check clean host-test md md-test samples-roms
all: $(ROM) $(MDROM)

$(OBJ) $(OBJ)/sh $(OBJ)/md $(OBJ)/host rom:
	mkdir -p $@

# ---- 68000 program ----
$(OBJ)/md_crt.o: src/md/md_crt.s | $(OBJ)
	$(MDAS) -m68000 --register-prefix-optional $< -o $@
$(OBJ)/midi_tables.h: tools/gen_tables.py | $(OBJ)
	python3 tools/gen_tables.py $@
$(OBJ)/md/%.o: src/md/%.c src/md/md.h src/common/comm.h $(OBJ)/midi_tables.h | $(OBJ)/md
	$(MDCC) $(MDCFLAGS) -c $< -o $@
$(OBJ)/md.bin: $(MD_OBJS) src/md/md.ld
	$(MDCC) -m68000 -nostdlib -T src/md/md.ld $(MD_OBJS) -lgcc -o $(OBJ)/md.elf -Wl,-Map=$(OBJ)/md.map
	python3 tools/check_ramtext.py $(MDPREFIX)objdump $(OBJ)/md.elf
	$(MDOBJC) -O binary $(OBJ)/md.elf $@
	@echo "68000 program: $$(stat -c %s $@) bytes"

# ---- SH-2 program ----
$(OBJ)/font.h: tools/gen_font.py tools/font5x7.py | $(OBJ)
	python3 tools/gen_font.py $@
$(OBJ)/crt0.o: src/32x/crt0.s $(OBJ)/md.bin src/32x/icd_mars.bin
	$(AS) --big -I$(OBJ) $< -o $@
$(OBJ)/synth_tables.h: tools/gen_synth_tables.py | $(OBJ)
	python3 tools/gen_synth_tables.py $@
$(OBJ)/sh/%.o: src/32x/%.c $(HEADERS) $(OBJ)/font.h $(OBJ)/synth_tables.h | $(OBJ)/sh
	$(CC) $(SHFLAGS) $< -o $@
$(OBJ)/sh/patch.o: src/common/patch.c src/common/patch.h | $(OBJ)/sh
	$(CC) $(SHFLAGS) $< -o $@
$(OBJ)/$(TARGET).elf: $(SH_OBJS) src/32x/mars.ld
	$(CC) $(LDFLAGS) $(SH_OBJS) $(LIBS) -o $@
	$(SIZE) $@
	@grep -E '^ +0x[0-9a-f]+ +__bss_end' $(OBJ)/$(TARGET).map || true
$(OBJ)/$(TARGET).raw: $(OBJ)/$(TARGET).elf
	$(OBJC) -O binary $< $@

$(ROM): $(OBJ)/$(TARGET).raw | rom
	cp $< $@
	python3 tools/romfix.py $@ --pow2 --gran 4194304   # 4 MiB: drum samples land at 0x100000-0x3FFFFF
	python3 tests/verify_rom.py $@ $(OBJ)/$(TARGET).elf $(MARKERS) --min-text 2048

# ---- the test ROM: patches built in (the emulator has no SD card) ----
$(OBJ)/testpatches/stamp: tests/make_test_patches.py | $(OBJ)
	rm -rf $(OBJ)/testpatches $(OBJ)/testdrums
	python3 tests/make_test_patches.py $(OBJ)/testpatches
	touch $@
test-rom: $(OBJ)/$(TARGET).raw $(OBJ)/testpatches/stamp | rom
	python3 tools/mkvfs.py $(OBJ)/$(TARGET).raw $(TESTROM) $(OBJ)/testpatches $(OBJ)/testdrums
	python3 tools/romfix.py $(TESTROM) --pow2 --gran 524288
	python3 tests/verify_rom.py $(TESTROM) $(OBJ)/$(TARGET).elf $(MARKERS) --marker "PATFS@" --min-text 2048

# ---- the Genesis-only build: no 32X, the screens on the 68000 too ----
# (src/32x/ui.c guide.c lib.c over src/mdonly/: COMM in RAM, a tile canvas)
MDROM     := rom/midimd.bin
MDTESTROM := rom/midimd-test.bin
MDO_SRC   := src/md/md_main.c src/md/md_rx.c src/md/md_snd.c src/md/md_midi.c \
             src/md/md_files.c src/md/md_ed.c src/md/md_vfs.c \
             src/32x/ui.c src/32x/viz.c src/32x/guide.c src/32x/lib.c src/32x/libc_min.c src/common/patch.c \
             src/mdonly/hw_md.c src/mdonly/gfx_md.c src/mdonly/synth_stub.c
MDO_OBJS  := $(OBJ)/mdonly/crt.o $(patsubst %.c,$(OBJ)/mdonly/%.o,$(notdir $(MDO_SRC)))
MDOFLAGS   = $(MDCFLAGS) -DMD_ONLY -Isrc/mdonly -Isrc/md -Isrc/32x \
             -fno-delete-null-pointer-checks -fno-tree-loop-distribute-patterns
vpath %.c src/md src/32x src/common src/mdonly
$(OBJ)/mdonly:
	mkdir -p $@
$(OBJ)/mdonly/crt.o: src/mdonly/crt.s | $(OBJ)/mdonly
	$(MDAS) -m68000 --register-prefix-optional $< -o $@
$(OBJ)/mdonly/%.o: %.c $(HEADERS) $(wildcard src/mdonly/*.h) src/md/md.h $(OBJ)/font.h $(OBJ)/midi_tables.h | $(OBJ)/mdonly
	$(MDCC) $(MDOFLAGS) -c $< -o $@
$(OBJ)/midimd.bin: $(MDO_OBJS) src/mdonly/mdonly.ld
	$(MDCC) -m68000 -nostdlib -T src/mdonly/mdonly.ld $(MDO_OBJS) -lgcc -o $(OBJ)/midimd.elf -Wl,-Map=$(OBJ)/midimd.map
	$(MDOBJC) -O binary $(OBJ)/midimd.elf $@
	@grep -E '^ +0x[0-9a-f]+ +__bss_end' $(OBJ)/midimd.map
$(MDROM): $(OBJ)/midimd.bin | rom
	cp $< $@
	python3 tools/romfix.py $@ --pow2 --gran 131072
md: $(MDROM)
# the sample patches (tools/make_sample_patches.py) built in: long lists
SAMPLEROM   := rom/midi32x-samples.32x
MDSAMPLEROM := rom/midimd-samples.bin
$(OBJ)/samples/stamp: tools/make_sample_patches.py | $(OBJ)
	rm -rf $(OBJ)/samples
	python3 tools/make_sample_patches.py $(OBJ)/samples
	touch $@
samples-roms: $(OBJ)/$(TARGET).raw $(OBJ)/midimd.bin $(OBJ)/samples/stamp | rom
	python3 tools/mkvfs.py $(OBJ)/$(TARGET).raw $(SAMPLEROM) $(OBJ)/samples
	python3 tools/romfix.py $(SAMPLEROM) --pow2 --gran 524288
	python3 tools/mkvfs.py $(OBJ)/midimd.bin $(MDSAMPLEROM) $(OBJ)/samples
	python3 tools/romfix.py $(MDSAMPLEROM) --pow2 --gran 131072
md-test: $(OBJ)/midimd.bin $(OBJ)/testpatches/stamp | rom
	python3 tools/mkvfs.py $(OBJ)/midimd.bin $(MDTESTROM) $(OBJ)/testpatches
	python3 tools/romfix.py $(MDTESTROM) --pow2 --gran 131072

# ---- tests ----
$(OBJ)/host/test_patch: tests/host/test_patch.c src/common/patch.c src/common/patch.h | $(OBJ)/host
	$(HOSTCC) -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -Isrc/common \
	    -o $@ tests/host/test_patch.c src/common/patch.c
$(OBJ)/host/test_midi: tests/host/test_midi.c src/md/md_midi.c src/md/md.h $(OBJ)/midi_tables.h | $(OBJ)/host
	$(HOSTCC) -O1 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
	    -Isrc/md -Isrc/common -I$(OBJ) -o $@ tests/host/test_midi.c src/md/md_midi.c
$(OBJ)/host/test_link: tests/host/test_link.c tests/host/host_md.h src/md/md_rx.c src/md/md.h | $(OBJ)/host
	$(HOSTCC) -O1 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
	    -DMD_HOST -Itests/host -Isrc/md -Isrc/common -o $@ tests/host/test_link.c src/md/md_rx.c
host-test: $(OBJ)/host/test_patch $(OBJ)/host/test_midi $(OBJ)/host/test_link
	$(OBJ)/host/test_patch
	$(OBJ)/host/test_midi
	$(OBJ)/host/test_link

tests/harness: tests/harness.c
	$(HOSTCC) -O2 -std=c11 -Wall -o $@ $< -ldl -lm

check: $(ROM) test-rom $(MDROM) md-test samples-roms host-test tests/harness
	@if [ -f tests/emu/picodrive_libretro.so ]; then python3 tests/run_tests.py; \
	else echo "SKIP emulator tests: put picodrive_libretro.so in tests/emu (see tests/emu/README.md)"; fi

clean:
	rm -rf $(OBJ) rom tests/out tests/harness
