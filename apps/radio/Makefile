APP_ID := radio
APP_SRCS := src/main.c src/radio_client.c src/radio_store.c src/radio_player.c \
	../../lib/streaming/http.c ../../lib/streaming/tls.c ../../lib/streaming/hls.c \
	../../lib/streaming/decoders/dr_mp3_impl.c ../../lib/streaming/decoders/aacdec.c
APP_INCLUDES := -I../../lib/streaming -I../../lib/streaming/decoders
APP_LIBS := -lpthread -ldl
# http.c uses strcasestr and MSG_NOSIGNAL.
APP_CFLAGS := -D_GNU_SOURCE
# The root page builds nine rows and BACK leaves; network work happens only
# when a destination is opened.
APP_TEST_SCRIPT := sleep:300; press:back

MORPTHIS_SDK ?= $(abspath ../../../morpthis-sdk)
include $(MORPTHIS_SDK)/app.mk

ifeq ($(MORPTHIS_MODE),host)
export MORPTHIS_APP_DATA := $(abspath $(BUILD_DIR)/data)
$(BUILD_DIR)/data:
	@mkdir -p "$@"
test: $(BUILD_DIR)/data

# The loopback test links the app's client and transport, not the UI. The SDK
# calls the transport makes are stubbed in the test; integration.py serves the
# directory, an MP3 stream and an HLS manifest.
TEST_OBJS := $(OBJ_DIR)/src/radio_client.o $(OBJ_DIR)/src/radio_player.o \
	$(OBJ_DIR)/../../lib/streaming/http.o $(OBJ_DIR)/../../lib/streaming/tls.o \
	$(OBJ_DIR)/../../lib/streaming/hls.o \
	$(OBJ_DIR)/../../lib/streaming/decoders/dr_mp3_impl.o \
	$(OBJ_DIR)/../../lib/streaming/decoders/aacdec.o

test-integration: $(TEST_OBJS) tests/test_radio.c tests/integration.py
	@mkdir -p $(BUILD_DIR)
	$(MORPTHIS_CC) $(MORPTHIS_CFLAGS) -Isrc -o $(BUILD_DIR)/test_radio \
		tests/test_radio.c $(TEST_OBJS) $(APP_LIBS)
	python3 tests/integration.py $(BUILD_DIR)/test_radio

.PHONY: test-integration
endif
