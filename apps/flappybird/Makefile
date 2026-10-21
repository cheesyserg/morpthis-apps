APP_ID := flappybird
APP_SRCS := src/main.c
APP_ASSETS := assets
APP_LIBS := -lpthread
# The game's clock uses nanosleep and pthreads; keep the build independent of
# whether the SDK's device flags already expose POSIX.
APP_CFLAGS := -D_POSIX_C_SOURCE=200809L
APP_TEST_SCRIPT := press:playpause; sleep:40; press:playpause; sleep:40; press:back

MORPTHIS_SDK ?= $(abspath ../../../morpthis-sdk)
include $(MORPTHIS_SDK)/app.mk
