# Doom

The classic Doom engine on the player's Apps page, built from
[doomgeneric](https://github.com/ozkl/doomgeneric) and driven by the Morpthis
SDK.

Doom runs on a thread of the app's own. Its 320x200 indexed frame is expanded
through Doom's palette into RGB565 and scaled to the panel's 4:3 viewport; the
GUI thread blits that into a `morpthis_canvas_create()` canvas and never calls
into the game. Input goes the other way: the touch controls and physical keys
push Doom key events into a queue that `DG_GetKey()` drains.

## Game data

The repository does not contain a WAD. Copy one of these beside the app binary
in `Apps/doom/` on the card:

- `doom1.wad` — the freely distributable shareware version
- `doom.wad` or `doom2.wad` — a copy you own
- `freedoom1.wad` or `freedoom2.wad` — [Freedoom](https://freedoom.github.io/),
  freely licensed
- `plutonia.wad`, `tnt.wad`, `chex.wad`, `hacx.wad`

The shareware `doom1.wad` is enough to play the first episode. Doom reads the
WAD on demand into a 6 MB zone; the zone and the WAD handles are released when
the app leaves, so relaunching does not leak the player's memory.

## Controls

The top of the panel is the game; the controls are below it.

| Control | What it does |
| --- | --- |
| D-pad | Up/down moves, left/right turns; diagonals work |
| `FIRE` | Fire the weapon; a tap on the picture also fires |
| `USE` | Open doors, press switches |
| `RUN` | Hold to run |
| `MENU` | Escape: opens and closes Doom's menu |
| `OK` | Enter: selects a menu item, and answers yes to Doom's prompts |
| `X` | Leave the app |

In Doom's menu, `MENU` opens it, the D-pad moves, `OK` selects.
**Quit Game** asks `y/n`; `OK` sends `y` first, so it confirms.

The physical keys work too:

| Key | Action |
| --- | --- |
| PLAY/PAUSE | Fire (a short burst) |
| NEXT / PREV | Next / previous weapon |
| POWER | Menu |
| BACK | Leave the app, when the player delivers it |

The player reports the transport keys as press-only, so each press is turned
into a short hold and released by the port itself. Volume keys stay the
player's.

## Saving

Doom writes savegames under `.savegame/` in the app folder. `doomgeneric` is
built without `ORIGCODE`, so it does not persist `default.cfg`; settings such
as gamma and screen size reset on each launch. Savegames do survive.

## Sound

This build is silent. `doomgeneric`'s sound support is the SDL mixer, which is
not available here; wiring Doom's `sound_module_t` to `morpthis_audio_open()`/
`write()` is a possible follow-up. The game is played with `-nosound
-nomusic`.

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
drawing and the app lifecycle.

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
| `m_config.c` | The default config dir is the app folder, not the player's working directory |
| `doomgeneric.c` | `DG_ScreenBuffer` is sized by `sizeof(pixel_t)` so `CMAP256` uses one byte per pixel |
| `z_zone.c` / `z_zone.h` | `Z_Shutdown()` releases the zone when the plugin is unloaded |
| `w_wad.c` | A WAD that fails validation is closed before the error unwinds |

## Licence

Doom's source is GPLv2-or-later; this app is distributed under the
repository's GPLv3 licence, which those terms allow. The game data is not part
of the repository. Freedoom is freely licensed; shareware `doom1.wad` is
freely distributable. `doom.wad` and `doom2.wad` are not.
