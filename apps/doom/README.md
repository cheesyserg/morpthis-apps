# doomgeneric

The classic Doom engine on the player's Apps page, built from
[doomgeneric](https://github.com/ozkl/doomgeneric) and driven by the Morpthis
SDK.

Doom runs on a thread of the app's own. Its 320x200 indexed frame is expanded
through Doom's palette into RGB565 and scaled to a 4:3 landscape rectangle; the
port rotates that rectangle into the panel's portrait canvas, so the player is
held sideways with its top edge on the right. The controls are translucent and
sit over the picture, using the whole screen for the game. Input goes the other
way: the touch controls and physical keys push Doom key events into a queue
that `DG_GetKey()` drains.

## Game data

The repository does not contain a WAD. Copy one of these beside the app binary
in `Apps/doom/` on the card:

- `doom1.wad` — the freely distributable shareware version
- `doom.wad` or `doom2.wad` — a copy you own
- `freedoom1.wad` or `freedoom2.wad` — [Freedoom](https://freedoom.github.io/),
  freely licensed
- `plutonia.wad`, `tnt.wad`, `chex.wad`, `hacx.wad`

The shareware `doom1.wad` is enough to play the first episode. Before Doom
starts, the app checks that the candidate is a real IWAD with `PLAYPAL`,
`STBAR` and a map (`E1M1`, or `MAP01` for Doom II). A PWAD, a partial download
or a modified/re-release WAD that lacks those is skipped and named on the
error screen instead of failing inside the engine with `STBAR not found`. If a
bad file sits next to a good one, the good one is used.

Doom reads the WAD on demand into a 6 MB zone and the mixer keeps a 4 MB cache
of resampled sound effects; the zone, the sound cache and the WAD handles are
released when the app leaves, so relaunching does not leak the player's memory.

The official shareware `doom1.wad` is 4,196,020 bytes and begins with `IWAD`.
If your copy is shorter, it is a partial download or a different file; the
error screen reports the size it found.

## Controls

Hold the player landscape, with its top edge on the right. The game fills the
screen; the controls are drawn over it, semi-transparent so the picture stays
readable underneath.

| Control | What it does |
| --- | --- |
| D-pad | Up/down moves forward/back, left/right strafes; diagonals work |
| `FIRE` | Fire the weapon; a tap on the picture also fires |
| `USE` | Open doors, press switches |
| `RUN` | Hold to run |
| `MENU` | Escape: opens and closes Doom's menu |
| `OK` | Enter: selects a menu item, and answers yes to Doom's prompts |
| `X` | Leave the app |

In a menu, the D-pad navigates, `MENU` closes the menu, and `FIRE`/`PREV`
acts as OK. `USE`/`PLAY/PAUSE` goes back one level. The volume keys also move
up and down the current menu. **Quit Game** asks `y/n`; the OK action sends `y`
first, so it confirms.

### Save names

When Doom asks for a save name, the controls become a small T9-style editor:

- Volume up/down cycles through `SPACE`, `A-Z`, `0-9`, `-` and `.`. Stop on a
  letter for a moment and it is typed automatically; `FIRE`/`PREV` types it at
  once.
- `PLAY/PAUSE` deletes the previous character.
- `NEXT` or `OK` finishes the name and saves.
- `POWER` cancels the name and returns to the menu.

### Volume menu

Doom's Options -> Sound Volume page now shows three percentages instead of
sliders:

- `MASTER` is the player's own volume.
- `GAME` is Doom's sound-effect volume.
- `MUSIC` is Doom's music volume. Music is not implemented yet, but the setting
  is kept for when it is.

Select a row with the up/down keys and change it with left/right.

The physical keys use the same landscape idea:

| Key | Action |
| --- | --- |
| PLAY/PAUSE | Use (open doors, press switches) |
| NEXT | Next weapon |
| PREV | Fire; hold to fire continuously |
| VOLUME UP / DOWN | Turn the camera right / left while playing |
| POWER | Menu |
| BACK | Leave the app, when the player delivers it |

The volume keys are contextual. While a level is being played they turn the
camera; volume up turns right and volume down turns left. In Doom's menu, on
the pause screen, on intermission/finale screens and while Doom is loading, the
game hands them back and they adjust the player's volume as usual. The app
tells the player which state it is in with `morpthis_input_set_gameplay()`;
this needs a player that implements that call.

The on-screen D-pad is movement: up/down walk, left/right strafe, and the
volume keys turn. The player reports the physical keys as repeated presses
rather than release events, so the port turns each press into a short hold and
lets repeats extend it. The previous-track key therefore fires and the volume
keys turn for as long as they are held.

## Saving

Doom writes savegames under `.savegame/` in the app folder. `doomgeneric` is
built without `ORIGCODE`, so it does not persist `default.cfg`; settings such
as gamma and screen size reset on each launch. Savegames do survive.

## Sound

Sound effects are mixed in the app and written straight to the player's DAC
through `morpthis_audio_open()`/`morpthis_audio_write()`. Doom's WAD stores
effects as unsigned 8-bit mono at their own rate; the port resamples each one
once to 16-bit 44.1 kHz, caches it in a 4 MB LRU cache, and mixes up to sixteen
channels with Doom's volume and stereo separation. The mixer has its own
thread because writes block at the speed the DAC plays.

Music is not implemented yet, so the game runs with `-nomusic`. Taking the raw
output stops the player's playback; it does not resume by itself when Doom
exits, exactly as with any other app that opens the DAC.

## Building

The doomgeneric checkout is expected beside `morpthis-apps` and `morpthis-sdk`:

```text
morpthis/
  morpthis-apps/
  morpthis-sdk/
  doomgeneric/
```

`make` uses `../../../doomgeneric/doomgeneric`; set `DOOMGENERIC_DIR` to point
somewhere else. The checkout must be patched with `doomgeneric-morpthis.patch`
(the CI applies it to the pinned upstream commit); the Makefile refuses an
unpatched clone with a clear error. Then:

```sh
make MORPTHIS_MODE=host          # native build
make MORPTHIS_MODE=host test     # smoke test (no WAD -> the "missing WAD" screen)
make MORPTHIS_MODE=device        # MIPS plugin
make MORPTHIS_MODE=device check-abi
```

The app is a `plugin`, `category: game` and `tier: heavy`: it owns the panel
and asks the player to release playback and caches before loading.

## Port notes

`src/doom_port.c` implements doomgeneric's platform interface (`DG_Init`,
`DG_DrawFrame`, `DG_SleepMs`, `DG_GetTicksMs`, `DG_GetKey`, `DG_SetWindowTitle`)
and the `MORPTHIS_PORT` hooks. `src/main.c` owns the SDK canvas, the control
drawing and the app lifecycle. `src/doom_sound.c` supplies doomgeneric's
`sound_module_t`: it loads and resamples WAD effects, mixes them, and runs the
blocking PCM writes on its own thread.

The port needs a few small changes in the doomgeneric sources, all guarded by
`MORPTHIS_PORT` or generic fixes. They are kept in
`doomgeneric-morpthis.patch`, which the repository's CI applies to the pinned
upstream checkout (`e257a09`). Apply it by hand to a local doomgeneric clone:

```sh
# from the directory that holds morpthis-apps/ and doomgeneric/
git -C doomgeneric apply morpthis-apps/apps/doom/doomgeneric-morpthis.patch
```

or set `DOOMGENERIC_DIR` to an already patched checkout.

| File | Change |
| --- | --- |
| `i_system.c` | `I_Quit()` tells the host to unload the game; `I_Error()` unwinds to the Doom thread instead of `exit()` |
| `d_main.c` | The ENDOOM screen does not call `exit(0)` |
| `i_sound.c` | The port supplies its own sound module instead of SDL_mixer; music stays disabled |
| `m_config.c` | The default config dir is the app folder, not the player's working directory |
| `doomgeneric.c` | `DG_ScreenBuffer` is sized by `sizeof(pixel_t)` so `CMAP256` uses one byte per pixel |
| `z_zone.c` / `z_zone.h` | `Z_Shutdown()` releases the zone when the plugin is unloaded |
| `w_wad.c` | A WAD that fails validation is closed before the error unwinds |

## Licence

Doom's source is GPLv2-or-later; this app is distributed under the
repository's GPLv3 licence, which those terms allow. The game data is not part
of the repository. Freedoom is freely licensed; shareware `doom1.wad` is
freely distributable. `doom.wad` and `doom2.wad` are not.
