APP_ID := flappybird
APP_SRCS := src/main.c
APP_ASSETS := assets
APP_LIBS := -lpthread
# The game's clock uses nanosleep and pthreads; keep the build independent of
# whether the SDK's device flags already expose POSIX.
APP_CFLAGS := -D_POSIX_C_SOURCE=200809L
# Tap Play, then start a game and let the bird land: the run covers the title,
# the fade, playing, dying and the score board.
APP_TEST_SCRIPT := sleep:400; touch:120,169; sleep:1200; touch:120,200; sleep:2400; press:back

MORPTHIS_SDK ?= $(abspath ../../../morpthis-sdk)
include $(MORPTHIS_SDK)/app.mk
