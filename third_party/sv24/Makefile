# Standalone hardware regression player built from the same V28.1 sources.
# From this directory: source ~/.yaul.env; make
ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
$(error Undefined YAUL_INSTALL_ROOT: source ~/.yaul.env)
endif
SH_PROGRAM := sv24_player
SH_BUILD_DIR := ../../obj-sv24-sample
SH_OUTPUT_DIR := ../../build/sv24-sample
include $(YAUL_INSTALL_ROOT)/share/build.pre.mk
SH_SRCS := main.c sv24_slice.c sv24_frame_v03.c sv24_vdp2_layout.c sv24_dual.c sv24_dma.c sv24_scu.c sv24_audio.c pcmsys.c scsp_dsp.c adx_dsp_sega.c
SH_CFLAGS += -I. -O3 -fno-lto -Wall
IP_VERSION := V0.901
IP_RELEASE_DATE := 20261007
IP_AREAS := JTUBKAEL
IP_PERIPHERALS := JAMKST
IP_TITLE := SV28.1 RGB24 PAL FIX
IP_MASTER_STACK_ADDR := 0x06004000
IP_SLAVE_STACK_ADDR := 0x06001E00
IP_1ST_READ_ADDR := 0x06004000
IP_1ST_READ_SIZE := 0
IMAGE_DIRECTORY := ../../build/sv24-sample/stage
IMAGE_1ST_READ_BIN := 0.BIN
include $(YAUL_INSTALL_ROOT)/share/build.post.iso-cue.mk
