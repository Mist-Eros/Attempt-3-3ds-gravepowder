#---------------------------------------------------------------------------------
# Gravepowder - HOST UI PREVIEW / VERIFICATION HARNESS (bottom screen only)
#
# Builds `preview.exe`, a headless page-tour driver that runs the REAL
# source/app.c + source/ui.c scene state machine against the REAL portable core
# (source/core/game.c + input.c + data.c), with libctru/citro2d/render3d
# replaced by the host shims in ./stub and ./plat_host.c.  The driver writes the
# citro2d draw list of every UI page to out/<page>.txt; render_ui.py replays
# them into 320x240 PNG mockups and runs the layout/legibility/baseline checks.
#
# This is NOT part of the 3DS build (source/render3d.c, source/main.c and
# source/audio_3ds.c are intentionally not compiled here).
#
# Usage (from the repo root):
#   make -C tools/preview CC="D:/tools/zig/zig.exe cc"          # build
#   make -C tools/preview CC="D:/tools/zig/zig.exe cc" run      # tour + checks
#   make -C tools/preview clean
#---------------------------------------------------------------------------------

CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra
CPPFLAGS += -Istub -I../../include -I../../source
LDLIBS ?= -lm
PYTHON ?= python

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

TARGET := preview$(EXE)

PREVIEW_SRC := main.c plat_host.c
GAME_SRC := ../../source/app.c \
            ../../source/ui.c \
            ../../source/audio.c \
            ../../source/core/game.c \
            ../../source/core/data.c \
            ../../source/core/input.c
HEADERS := plat_host.h \
           stub/3ds.h stub/citro2d.h stub/citro3d.h \
           ../../include/grave.h ../../include/app.h ../../include/audio.h \
           ../../source/app.h ../../source/ui.h

.PHONY: all run check clean

all: $(TARGET)

$(TARGET): $(PREVIEW_SRC) $(GAME_SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(PREVIEW_SRC) $(GAME_SRC) $(LDLIBS)

# Capture every UI page and run the renderer/checker (PNG mockups + layout /
# legibility checks + comparison against the committed Azahar baselines).
run: $(TARGET)
	./$(TARGET) --scenario all --out out
	$(PYTHON) render_ui.py --out out --baselines ../../docs/screenshots

# Same as run (alias kept for symmetry with the other preview harnesses).
check: run

clean:
	rm -f $(TARGET)
	rm -rf out out_zoom
