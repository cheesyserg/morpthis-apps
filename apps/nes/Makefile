APP_ID := nes
APP_SRCS := src/main.c src/nes.c src/nes_system.cpp
APP_ASSETS := cart.png LICENSE-InfoNES NOTICE-InfoNES
APP_LIBS := -lpthread -lm
APP_CFLAGS := -D_POSIX_C_SOURCE=200809L
# The library is up, the card scan finishes, and BACK leaves the app.
APP_TEST_SCRIPT := sleep:400; press:back

# InfoNES is expected beside morpthis-apps, like doomgeneric.
INFONES_DIR ?= $(abspath ../../../InfoNES)
APP_INCLUDES := -I$(INFONES_DIR)/src -Isrc

ifeq ($(MORPTHIS_MODE),host)
# Host-only: lets a smoke test auto-start the first ROM (the host UI cannot
# click a list row). Not compiled into the device build.
APP_CFLAGS += -DNES_HOST_TEST
endif

ifneq ($(filter clean,$(MAKECMDGOALS)),clean)
ifeq ($(wildcard $(INFONES_DIR)/src/InfoNES.cpp),)
$(error InfoNES not found at $(INFONES_DIR); set INFONES_DIR to its src/ parent)
endif
endif

MORPTHIS_SDK ?= $(abspath ../../../morpthis-sdk)
include $(MORPTHIS_SDK)/app.mk

# The emulator core. InfoNES_Mapper.cpp includes every mapper as one unit.
#
# InfoNES's pAPU divides by the timer value; the NES divides by timer + 1, so
# every note is sharp, most of all the high ones. Patch the private copy in the
# build tree rather than editing the shared checkout.
PATCHED_PAPU := $(BUILD_DIR)/nesobj/InfoNES_pAPU_patched.cpp

$(PATCHED_PAPU): $(INFONES_DIR)/src/InfoNES_pAPU.cpp
	@mkdir -p $(dir $@)
	sed -e 's|/ ApuC1Freq;|/ ( ApuC1Freq + 1 );|g' \
		-e 's|/ ApuC2Freq;|/ ( ApuC2Freq + 1 );|g' \
		-e 's|/ ApuC3Freq;|/ ( ApuC3Freq + 1 );|g' $< > $@
	@grep -q 'ApuC1Freq + 1' $@ && grep -q 'ApuC2Freq + 1' $@ && grep -q 'ApuC3Freq + 1' $@ || \
		{ echo "the InfoNES pAPU pitch patch did not apply"; rm -f $@; exit 1; }

NES_CORE_SRCS := $(INFONES_DIR)/src/InfoNES.cpp \
	$(INFONES_DIR)/src/K6502.cpp \
	$(INFONES_DIR)/src/InfoNES_Mapper.cpp
NES_OBJS := $(patsubst $(INFONES_DIR)/src/%.cpp,$(BUILD_DIR)/nesobj/%.o,$(NES_CORE_SRCS))
NES_OBJS += $(BUILD_DIR)/nesobj/InfoNES_pAPU.o

$(BUILD_DIR)/nesobj/%.o: $(INFONES_DIR)/src/%.cpp
	@mkdir -p $(dir $@)
	$(MORPTHIS_CXX) $(MORPTHIS_CXXFLAGS) -w -fno-exceptions -fno-rtti -MMD -MP -c $< -o $@

$(BUILD_DIR)/nesobj/InfoNES_pAPU.o: $(PATCHED_PAPU)
	$(MORPTHIS_CXX) $(MORPTHIS_CXXFLAGS) -w -fno-exceptions -fno-rtti -MMD -MP -c $< -o $@

# The core objects have to precede the libraries on the link line.
MORPTHIS_LIBS := $(NES_OBJS) $(MORPTHIS_LIBS)
$(APP_BIN): $(NES_OBJS)

-include $(NES_OBJS:.o=.d)

ifeq ($(MORPTHIS_MODE),host)
# Keep the host test's ROM folder out of the source tree.
export MORPTHIS_APP_DATA := $(abspath $(BUILD_DIR)/data)
$(BUILD_DIR)/data:
	@mkdir -p "$@"
test: $(BUILD_DIR)/data
endif
