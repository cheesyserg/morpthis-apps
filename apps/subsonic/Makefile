APP_ID := subsonic
APP_SRCS := src/main.c src/settings.c src/subsonic_client.c src/browse_worker.c src/cover_worker.c \
	src/stream_player.c \
	src/vendor/json.c src/vendor/md5.c src/vendor/http.c src/vendor/tls.c \
	src/vendor/dr_mp3_impl.c src/vendor/dr_flac_impl.c
APP_INCLUDES := -Isrc/vendor
APP_LIBS := -lpthread -ldl
# http.c and tls.c use strcasestr, res_init and MSG_NOSIGNAL.
APP_CFLAGS := -D_GNU_SOURCE
# The settings screen is reachable without a network or a server. More now
# leads to the app menu; Settings follows Now playing, Queue and Star.
APP_TEST_SCRIPT := sleep:200; click:More; sleep:100; click:Now playing; sleep:100; press:back; sleep:100; click:More; sleep:100; click:Settings; sleep:100; click:Back; sleep:100; click:Back; sleep:100; press:back

MORPTHIS_SDK ?= $(abspath ../../../morpthis-sdk)
include $(MORPTHIS_SDK)/app.mk

ifeq ($(MORPTHIS_MODE),host)
# Keep the host test's settings.json template out of the source tree. The SDK
# host runtime reads this, and build/ is ignored.
export MORPTHIS_APP_DATA := $(abspath $(BUILD_DIR)/data)
$(BUILD_DIR)/data:
	@mkdir -p "$@"
test: $(BUILD_DIR)/data
endif

# The loopback integration test. It needs no device and no external network:
# tests/integration.py serves a valid silent MP3 over 127.0.0.1, and the test
# binary stubs the SDK's audio and post calls. Kept out of the app's `test`
# target so the SDK smoke test stays dependency-free; the build workflow runs
# this as its own step.
ifeq ($(MORPTHIS_MODE),host)
TEST_OBJS := $(OBJ_DIR)/src/subsonic_client.o $(OBJ_DIR)/src/stream_player.o \
	$(OBJ_DIR)/src/vendor/json.o $(OBJ_DIR)/src/vendor/md5.o \
	$(OBJ_DIR)/src/vendor/http.o $(OBJ_DIR)/src/vendor/tls.o \
	$(OBJ_DIR)/src/vendor/dr_mp3_impl.o $(OBJ_DIR)/src/vendor/dr_flac_impl.o

test-integration: $(TEST_OBJS) tests/test_stream.c tests/integration.py
	@mkdir -p $(BUILD_DIR)
	$(MORPTHIS_CC) $(MORPTHIS_CFLAGS) -Isrc -o $(BUILD_DIR)/test_stream \
		tests/test_stream.c $(TEST_OBJS) $(APP_LIBS)
	python3 tests/integration.py $(BUILD_DIR)/test_stream

.PHONY: test-integration
endif
