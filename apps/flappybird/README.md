# Flappy Bird

A tap-to-flap game for the player's Apps page.

## Play

- Tap anywhere, or press PLAY/PAUSE, to flap. The first tap on Get Ready starts
  the game.
- The chevron (or the swipe-back gesture) leaves the game.
- The best score lives in `best.txt` beside the app on the card.
- Medals at 10 (bronze), 20 (silver) and 30 (gold).

The manifest says `heavy`: the game takes the DAC for its own effects, which
stops playback, so the player releases its audio path and caches before loading
it. The queue is kept.

## How it is built

The game is the player's own Flappy Bird page, ported to the SDK. The physics,
pipe spacing, gap range, medal thresholds and the artwork are the originals.

The renderer moved from one software framebuffer to widgets: the background,
ground, pipes and bird are image widgets moved on a tick, and the score digits
are created when the value changes (the SDK cannot swap an image source). The
bird tilts with `morpthis_widget_set_rotation`; a white full-screen image at 86%
is the hit flash; a black one fades between screens; both are kept on top with
`morpthis_widget_raise`.

API 1 has no timer, so a 16 ms thread of the app's own posts each tick with
`morpthis_app_post()`; every widget call still runs on the GUI thread.

## Sound

The five effects are the original Ogg files decoded to WAV and checked in under
`assets/sounds/`. A second thread mixes them into the raw output taken with
`morpthis_audio_open(44100, 2, 46)`: the write blocks at the rate the DAC plays,
which paces the 10 ms mix. The output is closed as the thread stops, before the
app leaves.

The PCM and the rotation/opacity/raise calls are API 1 additions, so this app
needs a player built from the matching SDK. It does not run on a player that
predates them.

## Artwork

From `morpthis-player` at commit `a57fa11`, under
`sonix-packer/assets/R1/usr/resource/sonix/gui/flappybird/`. The images are
scaled 2x for the app; `how-to.png` stays at 1x because the app's content area
is shorter than the player's full screen. The set is part of the player and
carries its GPLv3 licence.
