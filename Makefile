# Plain-make alternative to CMakeLists.txt (see run.sh for the CMake/Ninja build).
CC      ?= cc
STD     := -std=gnu11
WARN    := -Wall -Wextra -Wno-unused-parameter
OPT     := -O2 -g
PKGS    := sdl3 vorbisfile libavcodec libswscale libavutil
CFLAGS  += $(STD) $(WARN) $(OPT) $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

SRC := $(wildcard src/*.c)
OBJDIR := obj
OBJ := $(SRC:src/%.c=$(OBJDIR)/%.o)
BIN := saber_rider

.PHONY: all clean run
all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDLIBS)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

run: $(BIN)
	SABER_ASSETS="$(CURDIR)/assets" ./$(BIN) ../SaberRider/data

clean:
	rm -rf $(OBJDIR) $(BIN)
