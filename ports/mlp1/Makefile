# Disco Boy MLP1 cross-build. Runs inside the mlp1-toolchain container with the
# UMRK workspace mounted at /workspace (DiscoBoy, Catastrophe, Jawaka as siblings).
TC       := /opt/mlp1-toolchain
CC       := $(TC)/bin/aarch64-buildroot-linux-gnu-gcc
SYSROOT  := $(TC)/aarch64-buildroot-linux-gnu/sysroot
DISCOBOY ?= /workspace/DiscoBoy
CATA     ?= /workspace/Catastrophe
CJSON    ?= /workspace/Jawaka/third_party/cjson
SRC      := $(DISCOBOY)/cmd/discoboy
OBJDIR   := $(DISCOBOY)/ports/mlp1/build
OUT      := $(DISCOBOY)/ports/mlp1/pak/bin/discoboy

INCLUDES := -I$(SRC) -I$(DISCOBOY)/third_party -I$(CATA)/include -I$(CJSON) \
            -I$(SYSROOT)/usr/include -I$(SYSROOT)/usr/include/SDL2
COMMON   := -O2 -DPLATFORM_MLP1 $(INCLUDES)
APP_CFLAGS    := -std=gnu11 -Wall -Wextra -Wno-unused-parameter $(COMMON)
VENDOR_CFLAGS := -std=gnu11 -w $(COMMON)   # dr_*, stb_vorbis, cJSON: third-party, silence noise
# Audio output is libasound (ALSA) direct, so it can target the `bluealsa` pcm for
# Bluetooth the same way the RetroArch runner does. SDL is still linked for the UI.
LDFLAGS  := -L$(SYSROOT)/usr/lib -lSDL2 -lSDL2_ttf -lSDL2_image -lasound -lm -lpthread

# Each decoder's IMPLEMENTATION lives in its own TU (compiled -w); disco_audio.c
# includes only the declaration side of each header (stb_vorbis via HEADER_ONLY).
OBJ := $(OBJDIR)/main.o $(OBJDIR)/disco_audio.o $(OBJDIR)/disco_meta.o $(OBJDIR)/disco_status.o \
       $(OBJDIR)/dr_wav_impl.o $(OBJDIR)/dr_mp3_impl.o $(OBJDIR)/dr_flac_impl.o \
       $(OBJDIR)/stb_vorbis.o $(OBJDIR)/cJSON.o

.PHONY: all clean
all: $(OUT)

$(OUT): $(OBJ)
	mkdir -p $(dir $(OUT))
	$(CC) -o $(OUT) $(OBJ) $(LDFLAGS)

$(OBJDIR)/main.o: $(SRC)/main.c | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<
$(OBJDIR)/disco_audio.o: $(SRC)/disco_audio.c | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<
$(OBJDIR)/disco_meta.o: $(SRC)/disco_meta.c | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<
$(OBJDIR)/disco_status.o: $(SRC)/disco_status.c | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<
$(OBJDIR)/dr_wav_impl.o: $(SRC)/dr_wav_impl.c | $(OBJDIR)
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<
$(OBJDIR)/dr_mp3_impl.o: $(SRC)/dr_mp3_impl.c | $(OBJDIR)
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<
$(OBJDIR)/dr_flac_impl.o: $(SRC)/dr_flac_impl.c | $(OBJDIR)
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<
$(OBJDIR)/stb_vorbis.o: $(DISCOBOY)/third_party/stb_vorbis.c | $(OBJDIR)
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<
$(OBJDIR)/cJSON.o: $(CJSON)/cJSON.c | $(OBJDIR)
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(OUT)
