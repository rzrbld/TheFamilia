# The Familia build. Host:  make            (x86-64, for bring-up)
#                   Device: make ARCH=aarch64 (inside docker familia-cross:bookworm)
ARCH   ?= host
GEN    ?= build/gen
OUT    := build/$(ARCH)
ifeq ($(ARCH),aarch64)
CC     := aarch64-linux-gnu-gcc
TUNE   := -mcpu=cortex-a53
else
CC     ?= cc
TUNE   :=
endif
OPT    ?= -O2
MEMBASE ?= 0x100000000ull
PC     := third_party/pcrecomp
CFLAGS := $(if $(filter 1,$(ICALL_TRACE)),-DRECOMP_ICALL_TRACE) $(if $(filter 1,$(TRACK_CUR_FUNC)),-DRECOMP_TRACK_CUR_FUNC) $(OPT) -g1 $(TUNE) -fno-strict-aliasing -fwrapv -DRECOMP_MEM_BASE=$(MEMBASE) \
          -I$(PC)/recomp32 -I$(PC)/win32hle -Iruntime $(shell pkg-config --cflags sdl2 SDL2_ttf)
LOCAL_REGS ?= 1
NONVOLATILE_MEM ?= 1
ICALL_TRACE ?= 0
TRACK_CUR_FUNC ?= 0
GENFLAGS := $(CFLAGS) -w $(if $(filter 1,$(LOCAL_REGS)),-DRECOMP_LOCAL_REGS) $(if $(filter 1,$(NONVOLATILE_MEM)),-DRECOMP_NONVOLATILE_MEM)
FFMPEG ?= build/ffmpeg
LIBS   := $(shell pkg-config --libs sdl2 SDL2_ttf vorbisfile) $(addprefix $(FFMPEG)/lib/lib,avformat.so avcodec.so swscale.so swresample.so avutil.so) -Wl,-rpath-link,$(FFMPEG)/lib -lpthread -lm -ldl

HLE_SRC := $(filter-out %_selftest.c,$(wildcard $(PC)/win32hle/*.c))
RT_EXCLUDE ?=
RT_SRC  := $(filter-out $(RT_EXCLUDE),$(wildcard runtime/*.c))
GEN_SRC := $(wildcard $(GEN)/*/f_*.c) $(GEN)/dispatch.c $(GEN)/probes.c

# PGO=gen: instrumented build (counters written to PGO_DIR on the device via
# GCOV_PREFIX, see docs); PGO=use: optimised with those counters.
PGO ?=
PGO_DIR ?= /src/build/pgo-data
ifeq ($(PGO),gen)
CFLAGS += -fprofile-generate=$(PGO_DIR) -fprofile-update=single -DFAMILIA_PGO_GEN
GENFLAGS += -fprofile-generate=$(PGO_DIR) -fprofile-update=single
LIBS += -fprofile-generate=$(PGO_DIR)
endif
ifeq ($(PGO),use)
CFLAGS += -fprofile-use=$(PGO_DIR) -fprofile-partial-training -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch
GENFLAGS += -fprofile-use=$(PGO_DIR) -fprofile-partial-training -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch
endif

obj = $(patsubst %.c,$(OUT)/%.o,$(1))
OBJS := $(call obj,$(HLE_SRC)) $(call obj,$(RT_SRC)) $(call obj,$(GEN_SRC))

$(OUT)/familia: $(OBJS)
	$(CC) -o $@ $^ $(LIBS)

$(OUT)/$(GEN)/%.o: $(GEN)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -MMD -MP -I$(dir $<) -c $< -o $@

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wall -MMD -MP -c $< -o $@

$(OUT)/runtime/movie.o: CFLAGS += -I$(FFMPEG)/include

# native replacements must round exactly like the x87 sequence they replace
$(OUT)/runtime/native_math.o $(OUT)/runtime/native_frame.o $(OUT)/runtime/native_cull.o $(OUT)/runtime/native_crt.o $(OUT)/runtime/native_vtx.o $(OUT)/runtime/native_light.o $(OUT)/runtime/native_anim.o $(OUT)/runtime/native_ipol.o $(OUT)/runtime/native_misc.o $(OUT)/runtime/native_coll.o $(OUT)/runtime/native_render.o $(OUT)/runtime/native_clip.o: CFLAGS += -ffp-contract=off

# the PortMaster launcher (import + settings page)
launcher: build/familia-launcher.aarch64
build/familia-launcher.aarch64: launcher/familia_launcher.c launcher/font.h
	$(CC) -O2 -Wall -o $@ $< $(shell pkg-config --cflags --libs sdl2 SDL2_ttf)

# the first-run importer (unpacks the user's CDs, patch and Game.exe into data/)
import: build/familia-import.aarch64
build/familia-import.aarch64: tools/import/familia-import.c
	$(CC) -O2 -Wall -o $@ $<

clean:
	rm -rf $(OUT)
.PHONY: clean launcher

-include $(OBJS:.o=.d)
