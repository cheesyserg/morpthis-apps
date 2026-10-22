# Adding an app

## 1. The folder

```text
apps/myapp/
  app.json
  icon.png          optional
  src/main.c
  Makefile
```

The directory name is the app id and the package name on the card. Use lowercase
letters, digits, `.`, `_` and `-`; no spaces.

## 2. The manifest

```json
{
  "id": "myapp",
  "name": "My App",
  "version": "1.0.0",
  "entry": "myapp",
  "icon": "icon.png",
  "description": "What it does in one line",
  "min_player": 1
}
```

| Field | Required | Notes |
| --- | --- | --- |
| `id` | no | Defaults to the directory name; must match it. |
| `name` | no | Defaults to the id. |
| `version` | no | Semver. Defaults to empty. |
| `entry` | **yes** | Binary name, relative to the package root. |
| `icon` | no | Relative, square; 64×64 or 128×128. |
| `description` | no | One line, no trailing period. |
| `min_player` | no | Non-negative; 0 means no requirement. An app that uses API 3 calls (playback, library, system, wireless, net, absolute fs, dialogs) says `3`. |
| `category` | no | `app` (default) or `game`. Games get the whole panel. |
| `tier` | no | `light` (default) or `heavy`. Heavy releases playback and caches first. |

`entry` and `icon` are refused by the player if they are absolute or contain
`..` or a backslash. Keep them plain file names.

`category` is `app` for anything that uses the player's screens and widgets:
the title row, the chevron and the swipe-back gesture stay, and
`morpthis_ui_height()` is the content area below the title. `game` is for an
app that draws its own frames on a canvas and owns the panel: no title row, no
chevron, no edge swipe, and `morpthis_ui_height()` is the full display. A game
is left through a control of its own. Its only chrome is a swipe down from the
top edge, which peeks at the status bar for a second; a second swipe while the
bar is up opens the control centre.

`tier` is the memory contract. `light` is the normal case: the player keeps
playing, keeps its caches, and the app has to fit in what is free. `heavy`
stops playback, closes the decoder and PCM, releases the SQLite page caches and
trims the heap before the app starts; use it only for an app that genuinely
needs the room. Tier and category are independent: a heavy app still has the
player's chrome unless it asks for `game`, and a game can be light.

## 3. The Makefile

```make
APP_ID := myapp
APP_SRCS := src/main.c
APP_TEST_SCRIPT := click:Exit
MORPTHIS_SDK ?= $(abspath ../../../morpthis-sdk)
include $(MORPTHIS_SDK)/app.mk
```

`app.mk` supplies:

- `build/host/<id>`: a native binary linked with the SDK host runtime.
- `build/device/<id>`: a MIPS plugin (default) or executable.
- `stage`: `build/<mode>/app/<id>/` with the binary, `app.json` and `icon.png`.
- `check-abi`: refuses glibc > 2.22 or GLIBCXX > 3.4.21.
- `test`: runs the host build with `APP_TEST_SCRIPT`.

Set `APP_KIND := exe` for a headless tool with its own `main()`. Leave it as
`plugin` for a UI app that implements `morpthis_app_main()`.

## 4. The code

```c
#include <morpthis/morpthis.h>

MORPTHIS_DEFINE_API_VERSION();

int morpthis_app_main(const morpthis_app_t *app) {
    morpthis_logi("myapp", "started, api %d", morpthis_app_api_version(app));

    morpthis_screen_t *screen = morpthis_screen_create("My App");
    morpthis_widget_t *label = morpthis_label_create(screen, "Hello");
    morpthis_widget_set_pos(label, 20, 20);
    morpthis_widget_set_size(label, 200, 24);

    morpthis_widget_t *exit = morpthis_button_create(screen, "Exit", on_exit, NULL);
    morpthis_widget_set_pos(exit, 60, 180);
    morpthis_widget_set_size(exit, 120, 44);

    morpthis_screen_load(screen);
    return 0;
}
```

Rules from the SDK:

- The entry point sets up and returns. Do not block the GUI thread.
- Long work goes on an app-owned thread and comes back with
  `morpthis_app_post()`.
- UI calls run on the GUI thread.
- Do not allocate in the draw path.
- Use `morpthis_app_mem_available()` and fail early if the app does not fit.
- Paths passed to `morpthis_fs_*` stay inside the app. Reads see the package and
  the data folder as one tree; writes go to the data folder.
- If the app owns threads, a network connection or the raw audio output, export
  `morpthis_app_shutdown()` to stop them. The player calls it before unloading,
  including when USB mass-storage mode closes the app to share the card.
- The chevron and the swipe-back deliver BACK and then quit the app. An app with
  its own pages calls `morpthis_app_back_handled()` after loading the parent
  screen; one that keeps working off screen calls `morpthis_app_background()`
  instead. With neither, BACK is the way out.
- A music app that takes the raw output can publish what it plays with
  `morpthis_audio_set_track()`. The status bar and the control centre then follow
  the app, their transport buttons arrive as ordinary button events, and a tap
  on the card sends `MORPTHIS_BUTTON_NOW_PLAYING` so the app can show its own
  now-playing page.
- Log a start line and an exit line.

## 5. The icon

Any 64×64 or 128×128 PNG. `tools/make-icon.py` draws a placeholder:

```sh
python3 tools/make-icon.py apps/myapp/icon.png
```

App-specific artwork is better; this only keeps the folder complete.

## 6. Test it

```sh
make -C apps/myapp MORPTHIS_MODE=host
make -C apps/myapp MORPTHIS_MODE=host test
```

`APP_TEST_SCRIPT` is a semicolon-separated host script:

| Command | Effect |
| --- | --- |
| `click:<label>` | Clicks the visible button with that text |
| `press:<button>` | `play`, `next`, `prev`, `volup`, `voldown`, `power`, `back` |
| `sleep:<ms>` | Waits, then runs posted callbacks |
| `quit` | Ends the app |

A missing button fails the test with status 2. `MORPTHIS_RUN_MS` bounds a run
that does not quit. `MORPTHIS_APP_DATA` points the writable folder somewhere
other than the app folder, which is the default on the host.

## 7. Validate and package

```sh
python3 tools/validate-manifest.py apps/myapp
make package
```

The device build needs `mipsel-rockbox-linux-gnu-gcc` from the pinned
toolchain, and `mksquashfs` from `squashfs-tools`. `make check-abi` must pass
before a release. The package is `dist/<id>.pkgmerp`: an uncompressed squashfs
image with `app.json`, the icon and the entry binary at its root. Copy it to
`Apps/pkgmerps/<id>.pkgmerp` on the card.

Anything the app writes at runtime belongs in `Apps/merp_data/<id>/`, which the
SDK presents as the same tree: a read falls back from the data folder to the
package, and a write always lands in the data folder. That way an update can
replace the package without touching a save or a setting. Doom's WADs are the
first example: they are copied to `Apps/merp_data/doom/`, not into the image.

## 8. Release

Tag the release `<id>-v<version>` and attach the `.pkgmerp`. The notes say what
changed and which player build the app was tested on. Do not commit `dist/` or
`build/`.
