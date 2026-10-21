# Flappy Bird

A tap-to-flap game for the player's Apps page.

## Play

- Tap anywhere, or press PLAY/PAUSE, to flap. The first tap on Get Ready starts
  the game.
- The X on the title screen leaves the game; Menu on the score board returns to
  the title. There is no chevron and no swipe-back: the whole panel belongs to
  the game.
- Swipe down from the top edge to peek at the status bar for a second. A second
  swipe while it is up opens the control centre, which the game resumes under.
- The best score lives in `best.txt` and the sound switch in `sound.txt`, beside
  the app on the card.
- Medals at 10 (bronze), 20 (silver) and 30 (gold).

The manifest says `category: game` (no top bar, no chevron, full-height content)
and `tier: heavy`: the game takes the DAC for its own effects, which stops
playback, so the player releases its audio path and caches before loading it.
The queue is kept.

## How it is built

The game is the player's own Flappy Bird page, ported to the SDK. The physics,
pipe spacing, gap range, medal thresholds, artwork and the renderer are the
originals.

The panel is one `morpthis_canvas_create()` buffer in RGB565. The game runs its
own 60 Hz clock: a thread of the app's own posts a tick every 16 ms with
`morpthis_app_post()`, each tick advances one step, draws into the canvas and
invalidates just the rectangles that changed. While the bird is in the air only
the bird, the pipes and the score are redrawn above the background's flat band;
the ground is redrawn down to its last varying row. Everything scales by an
integer factor (4x on the 480x720 panel, 2x at 240x320) with nearest-neighbour
blits, so a pixel stays a square and a drawing is never resampled.

The bird turns with the same per-step rotation as the original, and the white
hit flash and fades are drawn into the same buffer rather than stacked widgets.

API 1 additions this app needs: the canvas calls, and the raw PCM output
(`morpthis_audio_open()`/`write()`/`close()`), so it needs a player built from
the matching SDK. It does not run on a player that predates them.

## Sound

The five effects are the original Ogg files decoded to WAV and checked in under
`assets/sounds/`. A second thread loads them, then takes the output with
`morpthis_audio_open(44100, 2, 46)` **on the first sound that actually plays**
(the title screen is silent and a muted game never asks): the write blocks at
the rate the DAC plays, which paces the 10 ms mix. The output is closed as the
thread stops, before the app leaves. With no sound card the game still runs.

The DAC is opened on a thread of the app's own and the mixer is joined on one
too, never on the GUI thread: an open or a write can be inside a driver that
takes seconds to answer, and the player must not wait there. The screen is
released once the mixer is gone.

## Artwork

From `morpthis-player` at commit `a57fa11`, under
`sonix-packer/assets/R1/usr/resource/sonix/gui/flappybird/`, at their original
1x pixel size. A transparent pixel in those PNGs is either black or white
underneath, so the app does not carry PNGs: `tools/pack-sprites.py` decodes them
into `assets/sprites/*.spr` (RGB565 plus a one-bit mask), which is also why
there is no fringe around a drawing. To regenerate after an art change:

```sh
tools/pack-sprites.py <art-dir> assets/sprites
```

The set is part of the player and carries its GPLv3 licence.
