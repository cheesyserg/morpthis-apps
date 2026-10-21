APP_ID := doom
APP_SRCS := src/main.c

MORPTHIS_SDK ?= $(abspath ../../../morpthis-sdk)
DOOMGENERIC_DIR ?= $(abspath ../../../doomgeneric/doomgeneric)

# clean must work even when the external checkout is not present.
ifneq ($(filter clean,$(MAKECMDGOALS)),clean)
ifeq ($(wildcard $(DOOMGENERIC_DIR)/doomgeneric.c),)
$(error doomgeneric not found at $(DOOMGENERIC_DIR); set DOOMGENERIC_DIR to its doomgeneric/ directory)
endif
ifeq ($(shell grep -c MORPTHIS_PORT $(DOOMGENERIC_DIR)/i_system.c 2>/dev/null),0)
$(error doomgeneric at $(DOOMGENERIC_DIR) is not patched; apply doomgeneric-morpthis.patch)
endif
endif

APP_INCLUDES := -I$(DOOMGENERIC_DIR)
APP_LIBS := -lpthread -lm
# The app has no WAD in the repository, so the smoke test runs the
# "missing WAD" screen and leaves through BACK.
APP_TEST_SCRIPT := sleep:200; press:back

# Doom's renderer is built at its native 320x200 and scaled by the port. CMAP256
# keeps DG_ScreenBuffer at one byte per pixel; the port expands the palette to
# RGB565 when it hands a frame to the canvas.
DOOM_CFLAGS := -O2 -g -std=gnu11 -D_DEFAULT_SOURCE \
	-DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
	-DNORMALUNIX -DLINUX -DMORPTHIS_PORT -DFEATURE_SOUND \
	-I$(DOOMGENERIC_DIR) -Isrc

include $(MORPTHIS_SDK)/app.mk

# Every core file from the linuxvt build except the platform and SDL/Allegro
# back ends. doomgeneric.c itself is the core; only doomgeneric_<platform>.c is
# a back end.
DOOM_SRCS := $(filter-out \
	$(DOOMGENERIC_DIR)/doomgeneric_allegro.c \
	$(DOOMGENERIC_DIR)/doomgeneric_emscripten.c \
	$(DOOMGENERIC_DIR)/doomgeneric_linuxvt.c \
	$(DOOMGENERIC_DIR)/doomgeneric_sdl.c \
	$(DOOMGENERIC_DIR)/doomgeneric_soso.c \
	$(DOOMGENERIC_DIR)/doomgeneric_sosox.c \
	$(DOOMGENERIC_DIR)/doomgeneric_win.c \
	$(DOOMGENERIC_DIR)/doomgeneric_xlib.c \
	$(DOOMGENERIC_DIR)/i_allegromusic.c \
	$(DOOMGENERIC_DIR)/i_allegrosound.c \
	$(DOOMGENERIC_DIR)/i_sdlmusic.c \
	$(DOOMGENERIC_DIR)/i_sdlsound.c \
	$(DOOMGENERIC_DIR)/gusconf.c \
	$(DOOMGENERIC_DIR)/icon.c, \
	$(wildcard $(DOOMGENERIC_DIR)/*.c))

DOOM_OBJS := $(patsubst $(DOOMGENERIC_DIR)/%.c,$(BUILD_DIR)/doomobj/%.o,$(DOOM_SRCS))
DOOM_PORT_OBJ := $(BUILD_DIR)/doom_port.o
DOOM_SOUND_OBJ := $(BUILD_DIR)/doom_sound.o

# The Doom objects must precede the libraries on the link line.
MORPTHIS_LIBS := $(DOOM_OBJS) $(DOOM_PORT_OBJ) $(DOOM_SOUND_OBJ) $(MORPTHIS_LIBS)
$(APP_BIN): $(DOOM_OBJS) $(DOOM_PORT_OBJ) $(DOOM_SOUND_OBJ)

$(BUILD_DIR)/doomobj/%.o: $(DOOMGENERIC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(MORPTHIS_CC) $(MORPTHIS_CFLAGS) $(DOOM_CFLAGS) -w -MMD -MP -c $< -o $@

$(DOOM_PORT_OBJ): src/doom_port.c src/doom_port.h
	@mkdir -p $(dir $@)
	$(MORPTHIS_CC) $(MORPTHIS_CFLAGS) $(DOOM_CFLAGS) -MMD -MP -c $< -o $@

$(DOOM_SOUND_OBJ): src/doom_sound.c
	@mkdir -p $(dir $@)
	$(MORPTHIS_CC) $(MORPTHIS_CFLAGS) $(DOOM_CFLAGS) -MMD -MP -c $< -o $@

-include $(DOOM_OBJS:.o=.d) $(DOOM_PORT_OBJ:.o=.d) $(DOOM_SOUND_OBJ:.o=.d)
