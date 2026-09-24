# Plain-make alternative to CMakeLists.txt (see run.sh for the CMake/Ninja build). Dreamcast: make -f Makefile.dc
CC      ?= cc
STD     := -std=gnu11
WARN    := -Wall -Wextra -Wno-unused-parameter
OPT     := -O2 -g
PKGS    := sdl3 vorbisfile libavcodec libswscale libavutil
CFLAGS  += $(STD) $(WARN) $(OPT) -Isrc $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

SRC := $(wildcard src/*.c) $(wildcard src/platform/sdl3/*.c) $(wildcard src/platform/common/*.c)
OBJDIR := obj
OBJ := $(SRC:src/%.c=$(OBJDIR)/%.o)
BIN := saber_rider

.PHONY: all clean run
all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDLIBS)

$(OBJDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJ:.o=.d)

run: $(BIN)
	SABER_ASSETS="$(CURDIR)/assets" ./$(BIN) SaberRider/data

clean:
	rm -rf $(OBJDIR) $(BIN)
