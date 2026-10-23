# morpthis-apps

The apps that run on the Morpthis player, built against
[morpthis-sdk](https://github.com/morpthis/morpthis-sdk).

An app is one `.pkgmerp` file on the card under `Apps/pkgmerps/`: an
uncompressed squashfs image holding `app.json`, an optional icon and the entry
binary. This repository holds the source for those images, builds them with the
SDK, validates the manifests and packages the result as a `.pkgmerp`.

**Status.** Foundation. `calculator` is an in-process UI app with the SDK;
`flappybird` is a game with its own clock and PCM mixer; `doom` runs the
Doom engine from a sibling `doomgeneric` checkout; `nes` runs InfoNES from a
sibling checkout, with a scanned ROM library and a landscape game canvas;
`checksum` is a headless tool that the player runs through its `fork`/`execve`
path; `subsonic` is a network streaming app with its own HTTP/TLS stack and an
MP3/FLAC decoder; `radio` is internet radio with a directory client, MP3 and
ADTS AAC, HLS, favourites and background play. They all run through the
player's app paths; see the SDK's `docs/player-integration.md`.

## Quick start

```sh
make validate      # check every app.json
make host          # build every app for the host
make test          # host build plus smoke tests
```

The SDK is expected beside this repository:

```text
morpthis/
  morpthis-apps/
  morpthis-sdk/
  morpthis-player/
```

`doom` also needs the doomgeneric checkout next to it:

```text
morpthis/
  morpthis-apps/
  morpthis-sdk/
  doomgeneric/
```

`nes` needs the InfoNES checkout next to it:

```text
morpthis/
  morpthis-apps/
  morpthis-sdk/
  InfoNES/
```

Set `MORPTHIS_SDK` if it lives elsewhere:

```sh
make MORPTHIS_SDK=/path/to/morpthis-sdk test
```

Set `DOOMGENERIC_DIR` if the patched doomgeneric checkout lives elsewhere; see
[apps/doom/README.md](apps/doom/README.md) for the patch and the game data.
Set `INFONES_DIR` if the InfoNES checkout lives elsewhere; see
[apps/nes/README.md](apps/nes/README.md) for the ROM folder and controls.

`subsonic` and `radio` also have loopback integration tests that need no
server:

```sh
make -C apps/subsonic MORPTHIS_MODE=host test-integration
make -C apps/radio MORPTHIS_MODE=host test-integration
```

## Layout

```text
apps/
  calculator/      app.json, icon.png, src/main.c, Makefile
  checksum/        app.json, src/main.c, Makefile
  doom/            app.json, icon.png, src/, Makefile
  flappybird/      app.json, icon.png, src/main.c, assets/, Makefile
  nes/             app.json, icon.png, src/, tools/, Makefile
  radio/           app.json, icon.png, src/, tests/, Makefile
  subsonic/        app.json, icon.png, src/, tests/, Makefile
lib/
  streaming/       shared HTTP/TLS, HLS, MP3 and AAC sources for streaming apps
tools/
  validate-manifest.py
  package.sh
  make-icon.py
docs/
  adding-an-app.md
```

Each app owns its manifest, icon, source and build. There is no shared
`main.c` and no registry to edit: adding an app is adding a directory, and the
top-level build loops over `apps/*`.

## Adding an app

1. Create `apps/<id>/` with `app.json`, `src/main.c`, a `Makefile`, and an
   `icon.png` if you have one.
2. Set `id` to the directory name, `entry` to the binary the Makefile produces,
   and `version` to a semver string.
3. Include the SDK build template:

   ```make
   APP_ID := myapp
   APP_SRCS := src/main.c
   MORPTHIS_SDK ?= $(abspath ../../../morpthis-sdk)
   include $(MORPTHIS_SDK)/app.mk
   ```

4. Write `morpthis_app_main()`, set up a screen and return. Log one line on
   start and one on exit.
5. `make test`, then `make package` with the cross toolchain and
   `squashfs-tools` installed.

The full checklist and the manifest fields are in
[docs/adding-an-app.md](docs/adding-an-app.md).

## Build modes

| Command | What it builds |
| --- | --- |
| `make host` | native binaries linked with the SDK host runtime |
| `make test` | the same, then runs each app's smoke test |
| `make device` | MIPS plugins/executables with the pinned toolchain |
| `make check-abi` | device build and the glibc/GLIBCXX check |
| `make package` | device build and a `.pkgmerp` per app in `dist/` |

A UI app has `APP_KIND := plugin` (the default): a shared object named after the
app. A headless tool sets `APP_KIND := exe`. The player loads plugins
in-process; tools still run as a child process.

## Manifest conventions

- `id` equals the directory name, lowercase, no spaces.
- `entry` is the binary name the build produces.
- `version` is semver.
- `icon` is `icon.png` (64×64 or 128×128).
- `description` is one sentence, no trailing period.
- `min_player` is the oldest player build the app was tested on.
- `category` is `app` (the default) or `game`. A game gets the whole panel:
  no title row, no chevron, no swipe-back.
- `tier` is `light` (the default) or `heavy`. Light apps keep the player's
  playback and caches; heavy apps release them before loading and are for the
  rare app that needs every kilobyte.

`tools/validate-manifest.py` enforces the fields the player reads and the path
rules it applies: `entry` and `icon` are relative, with no backslash and no
`..`.

## Packaging

```sh
make package
```

writes `dist/<id>.pkgmerp`. A package is an uncompressed squashfs image with the
app folder at its root:

```text
calculator.pkgmerp
  app.json
  icon.png
  calculator
```

Uncompressed is deliberate: the player mounts the image read-only through a
loop device, and the kernel then serves its data straight from the card through
the page cache. The file is larger than a zip; the player has more card than
RAM. The superblock has to name a compressor the device kernel knows, and that
kernel's rootfs uses LZO, so packages use `-comp lzo` with `-b 4096`. Every
block is still stored uncompressed; `-b 4096` only keeps the driver's one-block
input buffer small. `mksquashfs` from `squashfs-tools` must be on the build
machine.

The name carries no version. Copy `dist/<id>.pkgmerp` to
`Apps/pkgmerps/<id>.pkgmerp` on the card; the app's writable files already live
in `Apps/merp_data/<id>/`, outside the image, so replacing the package keeps
them. There is no install step in the player yet.

## Releases

One GitHub release per app version, with the `.pkgmerp` attached. The tag names
the app and the version (`calculator-v1.0.0`), and the notes say what changed
and the minimum player build. The player does not download anything.

## CI

- Every push and pull request validates every manifest, runs the host build
  and smoke tests, and dry-runs the packaging in host mode.
- Device builds live in `morpthis-build`. That workflow checks this repository
  out, builds the pinned MIPS toolchain from the player's `rockboxdev` scripts
  (restored from the Actions cache afterwards), runs the ABI check and packages
  the `.pkgmerp` files; no repository secret is required. Every package is
  uploaded as its own artifact, and the whole set is also uploaded as one zip.
- On a tag, `morpthis-build` attaches the packages to the matching GitHub
  release when one exists. The release tag names the app and version, for
  example `calculator-v1.0.0`.

## What not to put here

- ROMs, BIOS files, Doom WADs or other copyrighted game assets (Freedoom and
  the shareware `doom1.wad` are the exceptions; they are freely licensed, but
  still belong on the card rather than in the repository).
- Streaming credentials or API keys.
- A copy of the player's source. The SDK is the interface; the player is a
  separate repository.
- Build outputs. Releases carry the `.pkgmerp`; the repository carries the
  source.
