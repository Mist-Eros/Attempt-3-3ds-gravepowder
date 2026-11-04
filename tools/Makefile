# Gravepowder - headless gameplay sim (balance / tuning harness).
#
#   make -C tools CC="D:/tools/zig/zig.exe cc" run
#
# Reuses the real portable core; no 3DS headers, no window.

CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -pedantic
CPPFLAGS ?= -I../include -I../source/core
LDLIBS ?= -lm

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

TARGET := sim$(EXE)
CORE_SRC := ../source/core/game.c ../source/core/data.c ../source/core/input.c
HEADERS  := ../include/grave.h ../source/core/core_internal.h

.PHONY: all run clean

all: $(TARGET)

$(TARGET): sim.c $(CORE_SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ sim.c $(CORE_SRC) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) sim sim.exe
