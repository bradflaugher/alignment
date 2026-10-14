# ALIGNMENT - build & test
GBDK_HOME ?= /opt/gbdk
LCC       := $(GBDK_HOME)/bin/lcc
CC        ?= gcc
PYTHON    ?= python3
BUILD     := build
OBJ       := $(BUILD)/obj
ROM       := $(BUILD)/alignment.gb

CORE_SRC  := src/core/stage.c src/core/phys.c src/core/rng.c src/core/run.c
CORE_HDR  := $(wildcard src/core/*.h)
GB_SRC    := $(filter-out src/gb/assets.c src/gb/music_data.c,$(wildcard src/gb/*.c))
GB_ASM    := $(wildcard src/gb/*.s)
GB_HDR    := $(wildcard src/gb/*.h)
HOST_CFLAGS := -std=c99 -O2 -Wall -Wextra -Werror

# MBC5 + RAM + battery (0x1B), 4 x 8 KB SRAM banks (bank 0 = save, bank 1 = the
# solver's work area), CGB-enhanced, ROM banks packed automatically
CFLAGS_GB  := -Isrc/core -Isrc/gb
# the hot core gets the slow, thorough register allocator
CFLAGS_CORE := $(CFLAGS_GB) -Wf--max-allocs-per-node50000 -Wf--opt-code-speed
LDFLAGS_GB := -Wm-yt0x1B -Wm-ya4 -Wm-yc -Wm-yn"ALIGNMENT" -Wm-yoA -autobank -Wl-j -Wm-yS

GB_OBJS := $(patsubst src/core/%.c,$(OBJ)/core_%.o,$(CORE_SRC)) \
           $(patsubst src/gb/%.c,$(OBJ)/%.o,$(GB_SRC)) \
           $(OBJ)/assets.o $(OBJ)/music_data.o \
           $(patsubst src/gb/%.s,$(OBJ)/%_s.o,$(GB_ASM))

.PHONY: all rom assets music test test-host test-assets test-rom screenshots stats clean

all: rom

rom: $(ROM)

assets: src/gb/assets.c

src/gb/assets.c src/gb/assets.h &: $(wildcard assets/*.txt) tools/gen_assets.py
	$(PYTHON) tools/gen_assets.py

src/gb/music_data.c src/gb/music_data.h &: $(wildcard assets/music/*.py) tools/gen_music.py
	$(PYTHON) tools/gen_music.py

$(OBJ)/core_%.o: src/core/%.c $(CORE_HDR) | $(OBJ)
	$(LCC) $(CFLAGS_CORE) -c -o $@ $<

$(OBJ)/assets.o: src/gb/assets.c src/gb/assets.h | $(OBJ)
	$(LCC) $(CFLAGS_GB) -c -o $@ $<

$(OBJ)/music_data.o: src/gb/music_data.c src/gb/music_data.h | $(OBJ)
	$(LCC) $(CFLAGS_GB) -c -o $@ $<

$(OBJ)/%.o: src/gb/%.c $(CORE_HDR) $(GB_HDR) src/gb/assets.h src/gb/music_data.h | $(OBJ)
	$(LCC) $(CFLAGS_GB) -c -o $@ $<

$(OBJ)/%_s.o: src/gb/%.s | $(OBJ)
	$(LCC) -c -o $@ $<

$(ROM): $(GB_OBJS) | $(BUILD)
	$(LCC) $(LDFLAGS_GB) -o $@ $(GB_OBJS)
	@$(GBDK_HOME)/bin/romusage $(BUILD)/alignment.map -g 2>/dev/null | tail -n 8 || true
	@$(PYTHON) tools/check_bank0.py $(BUILD)/alignment.map || (rm -f $@; false)

$(BUILD):
	mkdir -p $(BUILD)

$(OBJ):
	mkdir -p $(OBJ)

# ---- host builds of the core
$(BUILD)/test_core: tests/test_core.c tests/ref_room.c tests/ref_room.h $(CORE_SRC) $(CORE_HDR) | $(BUILD)
	$(CC) $(HOST_CFLAGS) -Isrc/core -Itests -o $@ tests/test_core.c tests/ref_room.c $(CORE_SRC)

$(BUILD)/algen: tools/algen.c $(CORE_SRC) $(CORE_HDR) | $(BUILD)
	$(CC) $(HOST_CFLAGS) -Isrc/core -o $@ tools/algen.c $(CORE_SRC)

$(BUILD)/test_sound: tests/test_sound.c src/gb/sound.c src/gb/music_data.c $(GB_HDR) | $(BUILD)
	$(CC) $(HOST_CFLAGS) -DHOST_TEST -Isrc/gb -o $@ tests/test_sound.c src/gb/sound.c src/gb/music_data.c

test-host: $(BUILD)/test_core $(BUILD)/test_sound $(BUILD)/algen
	$(BUILD)/test_core
	$(BUILD)/test_sound

test-assets:
	$(PYTHON) -m unittest discover -s tests -p 'test_assets.py'

test-rom: $(ROM) $(BUILD)/algen
	$(PYTHON) -m unittest discover -s tests -p 'test_rom.py' -v

stats: $(BUILD)/algen
	$(BUILD)/algen stats 10 100

screenshots: $(ROM)
	$(PYTHON) tools/screenshots.py

test: test-host test-assets test-rom

clean:
	rm -rf $(BUILD)
