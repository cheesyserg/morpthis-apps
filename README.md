# morpthis-apps

The apps that run on the Morpthis player, built against
[morpthis-sdk](https://github.com/morpthis/morpthis-sdk).

An app is a folder under `Apps/` on the card: `app.json`, an optional icon, and
the entry binary. This repository holds the source for those folders, builds
them with the SDK, validates the manifests and packages the result as a
`.mapp`.

**Status.** Foundation. `calculator` is an in-process UI app with the SDK;
`checksum` is a headless tool that the player runs through its `fork`/`execve`
path. Both paths are in the player now; see the SDK's
`docs/player-integration.md`.

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

Set `MORPTHIS_SDK` if it lives elsewhere:

```sh
make MORPTHIS_SDK=/path/to/morpthis-sdk test
```

## Layout

```text
apps/
  calculator/      app.json, icon.png, src/main.c, Makefile
  checksum/        app.json, src/main.c, Makefile
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
5. `make test`, then `make package` with the cross toolchain.

The full checklist and the manifest fields are in
[docs/adding-an-app.md](docs/adding-an-app.md).

## Build modes

| Command | What it builds |
| --- | --- |
| `make host` | native binaries linked with the SDK host runtime |
| `make test` | the same, then runs each app's smoke test |
| `make device` | MIPS plugins/executables with the pinned toolchain |
| `make check-abi` | device build and the glibc/GLIBCXX check |
| `make package` | device build and a `.mapp` per app in `dist/` |

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

writes `dist/<id>-<version>.mapp`. A package is a zip with the app folder at its
root:

```text
calculator-1.0.0.mapp
  app.json
  icon.png
  calculator
```

Copy or extract it to `Apps/<id>/` on the card. There is no install step in the
player yet.

## Releases

One GitHub release per app version, with the `.mapp` attached. The tag names
the app and the version (`calculator-v1.0.0`), and the notes say what changed
and the minimum player build. The player does not download anything.

## CI

- Every push and pull request validates every manifest and runs the host build
  and smoke tests.
- Manual runs — and tags — build the device binaries, run the ABI check and
  package the `.mapp` files. That job needs the pinned toolchain; set the
  `MORPTHIS_TOOLCHAIN_URL` repository secret to the SDK's toolchain tarball.
- On a tag, the packages are also attached to the matching GitHub release when
  one exists. The release tag names the app and version, for example
  `calculator-v1.0.0`.

## What not to put here

- ROMs, BIOS files or copyrighted game assets.
- Streaming credentials or API keys.
- A copy of the player's source. The SDK is the interface; the player is a
  separate repository.
- Build outputs. Releases carry the `.mapp`; the repository carries the source.
