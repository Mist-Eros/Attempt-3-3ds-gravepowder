# Gravepowder - host unit tests for the portable core.
#
# The core is plain C99 with no 3DS headers, so it runs on the PC:
#
#   make -C tests CC="D:/tools/zig/zig.exe cc"        # build
#   make -C tests CC="D:/tools/zig/zig.exe cc" test   # build and run
#
# With a host compiler on PATH plain `make -C tests test` works too.

CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -pedantic
CPPFLAGS ?= -I../include -I../source/core
LDLIBS ?= -lm

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

TARGET := test_core$(EXE)

CORE_SRC := ../source/core/game.c ../source/core/data.c ../source/core/input.c
HEADERS  := ../include/grave.h ../source/core/core_internal.h

.PHONY: all test run clean

all: $(TARGET)

$(TARGET): test_core.c $(CORE_SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_core.c $(CORE_SRC) $(LDLIBS)

test run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) test_core test_core.exe test_core.exe.*

-include *.d
