# NES

A NES/Famicom player for Morpthis, built on [InfoNES](https://github.com/jay-kumogata/InfoNES)
and the SDK's own widgets. Games live on the card, the library scans them on
start, and the game screen is a full-bleed landscape picture with a touch pad.

The app ships no ROMs. Put `.nes` files in `Apps/merp_data/nes/roms/` on the
card, then tap the refresh button on the library page (the empty page also has
a **Rescan** button).

## Screens

- **Library.** A portrait list with the game name, mapper, PRG/CHR sizes,
  battery/trainer flags and a cartridge icon. The header has the back chevron,
  the game count, the add-games help and the refresh button. An empty card
  explains where the files go and offers **Rescan**.
- **Game.** The NES picture at an integer scale (1:1 on the V1), rotated so it
  reads upright when the player is held with its top edge on the right. The
  bottom corners carry a translucent D-pad disc and A/B buttons over the
  picture, in the same style as the Doom port; the pause target is in the
  top-right of the landscape picture. The overlay can be turned off from
  **Controls**; the physical buttons still work.
- **Pause.** Resume, Reset, Sound, Controls and Exit game. Opening it pauses the
  emulator at a scanline and silences the PCM output.
- **Controls.** The full mapping, plus a switch for the touch overlay.

The library's own chevron is the way out: the player gives a `game` app no
title row, no back affordance and no status bar. A downward swipe from the top
edge still peeks the status bar, as it does in every game.

## Controls

| Input | Action |
| --- | --- |
| Touch D-pad (left) | Directions, including diagonals |
| Touch A / B (right) | A / B |
| `VOLUME UP` | A |
| `VOLUME DOWN` | B |
| `PLAY/PAUSE` | Start |
| `NEXT` | Select |
| `PREV` | Turbo (hold) |
| `POWER` | Pause |
| `BACK` | Pause; resume from the pause page |
| Touch pause target | Pause |

The panel reports one touch point, so a touch starting on the pad owns the pad
until it lifts. The volume rocker is A and B, so the pad and the action buttons
can be held at the same time; `NEXT` and `PREV` carry Select and Turbo. The
transport keys are press-only on the V1, so the app turns a press into a short
hold and extends it while the player repeats.

Audio is mixed by InfoNES at 44.1 kHz and written straight to the player's PCM
output. The blocking write is the game clock; when sound is off or unavailable
the app paces frames itself. Taking the output stops the player's music and it
does not resume by itself when the app leaves.

## Battery saves

InfoNES writes the cartridge's SRAM, compressed, to
`Apps/merp_data/nes/saves/<game>.srm` about once a minute and when the game is
left. There are no save states; a game without battery-backed SRAM starts from
the beginning.

## Building

InfoNES is expected beside `morpthis-apps`, like `doomgeneric`:

```text
morpthis/
  morpthis-apps/
  morpthis-sdk/
  InfoNES/
```

```sh
make MORPTHIS_MODE=host          # native build
make MORPTHIS_MODE=host test     # headless smoke test
make MORPTHIS_MODE=device        # MIPS plugin
make MORPTHIS_MODE=device check-abi
```

Set `INFONES_DIR` if the checkout is elsewhere. The app compiles four core
translation units (`InfoNES.cpp`, `K6502.cpp`, `InfoNES_pAPU.cpp`,
`InfoNES_Mapper.cpp`) with the SDK's cross compiler and links the app's
`src/nes_system.cpp` against them. The core is C++ but uses no STL, exceptions
or RTTI.

The fork has no release tags; CI pins it to commit
`fe3295c0a86bf5bbecc22aeab3b3af11f6f47908` (2026-08-02), the revision this app is
tested against. Mapper behaviour differs between revisions, so pin yours too.

Two host-only aids are compiled only in `MORPTHIS_MODE=host` and never in the
device build: `NES_AUTOSTART=1` starts the first scanned ROM (the headless host
cannot click a list row), and `NES_DUMP_FRAME=/path/frame.bmp` writes the game
canvas once for inspection.

## Layout

```text
src/main.c          library, game canvas, overlay, pause and controls pages
src/nes.c           emulator thread, frame handoff, geometry, input atomics
src/nes_system.cpp  InfoNES system layer: ROM, SRAM, PCM, frame callback
src/nes.h           the boundary between the UI and the emulator
tools/make-icons.py icon.png and cart.png (standard library only)
```

The game picture is handed over as 256x240 RGB565. `nes.c` rotates it into the
player canvas at an integer scale; the GUI thread draws the touch controls over
it and invalidates the canvas. The emulator never calls into LVGL.

## Limits

- InfoNES is a lightweight, older core: expect occasional mapper or timing
  quirks that a cycle-accurate emulator would not have. Expansion audio (VRC6,
  VRC7, Namco 163, MMC5) is not emulated; the five standard APU channels are.
- Mapper support comes from the upstream InfoNES checkout: 138 mapper
  entries, including MMC1/3/5, VRC6/7, Namco 163 and FME-7.
- No save states, rewind, cheats or netplay.
- The touch panel is one point; the on-screen pad and A/B cannot be held at the
  same time. Use the volume rocker for A/B in those games.
- PAL games run at the core's NTSC timing.

## Licence

The app is distributed under the repository's GPLv3. InfoNES is Apache-2.0,
copyright Jay's Factory and the InfoNES contributors. The package carries
`LICENSE-InfoNES` and `NOTICE-InfoNES` beside the binary. ROMs and game data
are not part of this repository.
