# Subsonic

Streaming from a Subsonic-compatible server, packaged as an app.

This is the test the in-process app model needed: the player's built-in
Subsonic client, rebuilt outside the player and loaded from
`Apps/pkgmerps/subsonic.pkgmerp`. It browses Artists, Albums and Tracks, and
plays MP3 through the SDK's raw PCM output. It does its own HTTP, TLS and MP3
decoding; the player supplies only the display, input, files and DAC.

The built-in Subsonic page is still in the player. It stays until this app has
been through the device validation in the player's `docs/subsonic.md`; then it
can be removed.

API 1 deliberately has no networking calls. The app carries its own copy of the
player's HTTP/TLS stack as vendored sources. That is the point of the test: if
streaming belongs in an app, the next step is an SDK networking call rather
than every streaming app copying `http.c` and `tls.c`.

## Settings

API 1 has no text-entry widget, so the account lives in a file. On the first
launch the app writes a template to:

```text
<card>/merp_data/subsonic/settings.json
```

The app's data folder survives replacing the package. Edit the file from a PC
in USB mass-storage mode (the player offers to close apps gracefully first),
then start the app again, or use **More → Reload settings** if you changed it
over ADB while the app was running.

```json
{
  "server": "https://music.example.com",
  "username": "user",
  "password": "secret",
  "format": "mp3",
  "max_bitrate": 256,
  "tls_verify": "strict",
  "ca_bundle": ""
}
```

| Field | Notes |
| --- | --- |
| `server` | Base URL, with any reverse-proxy prefix. `http://` or `https://`; a pasted `/rest` suffix is removed. |
| `username`, `password` | The Subsonic account. The password is stored in plain text on the card; use a dedicated account. |
| `format` | Must be `mp3`. The app carries an MP3 decoder only, so the server must have transcoding enabled. |
| `max_bitrate` | 64–320 kbps, used only when `format` is `mp3`. The **Quality** button changes this. |
| `tls_verify` | `strict` (default) or `relaxed`. Relaxed accepts a self-signed certificate or an unknown issuer, but still checks the name and the dates. |
| `ca_bundle` | Optional absolute path to a PEM bundle. `<card>/.local/cacert.pem` and `/etc/ssl/cert.pem` are checked when this is empty. |

The quickest route to a working server:

- Turn on transcoding in Gonic/Navidrome/Subsonic; MP3 at 256 kbps is the
  default the app asks for.
- If the server has a self-signed certificate, set `"tls_verify": "relaxed"`
  or tap **TLS** on the settings screen. Prefer a private CA and `ca_bundle`
  where possible.

## Using it

The first screen is the artist list. Tapping an artist shows albums, an album
shows its tracks, and a track starts playing. The bottom row is **Up**,
**Stop** and **More**.

| Control | Action |
| --- | --- |
| **Up** | Goes back one level. At the top it says so. |
| **Stop** | Stops playback; the status line returns to the current view. |
| **More** | Opens the settings screen. |
| PLAY/PAUSE | Pauses and resumes the current stream. |
| NEXT / PREV | Moves through the tracks of the list that is open. |
| BACK | Leaves the app. |
| Volume | Still the player's volume, not the app's. |

The settings screen has **TLS: strict/relaxed**, **Quality: 256/320 kbps**,
**Reload settings** and **Back**. The first two write `settings.json`; they do
not restart the current track, so the next stream uses the new setting.

When a track ends, the app plays the next track in the list it was started
from. If you navigate to another list meanwhile, playback stays on the old
track and stops when it ends; open the album again to use NEXT/PREV there.

## Limits

This is a test of the model, not a finished music client:

- MP3 only. Opus and raw FLAC would need another decoder in the package.
- The first 200 entries of each view. There is no "load more" row yet.
- No search, cover art, seeking, gapless playback, scrobbling or playlists.
- One stream at a time. Starting another track stops the first.
- No look-ahead buffer: the decoder reads from the socket directly. A link
  that stalls can leave an audible gap. The player's built-in client uses two
  1 MiB RAM rings; this app deliberately does not, to keep its footprint down.
- Pausing keeps the socket open. A pause longer than the server's own timeout
  may end the track when it resumes.
- A lost network ends the track with an error; the app stays on its screen and
  can be used again. Pulling the card while the app runs is not a supported
  path: the package is mounted from it, and a fault in an in-process app takes
  the player down. The player's USB guard and confirmation exist to keep the
  card from being exported underneath it.
- IPv4 only, like the player's own network stack.
- The app asks for the `heavy` tier: the player stops its own playback and
  releases its caches before the app loads. The MP3 decoder and the JSON
  response are the app's main allocations.

## How it is put together

| File | Role |
| --- | --- |
| `src/main.c` | Screens, browsing state, playback queue, settings toggles. |
| `src/subsonic_client.c` | REST client: token auth, browse, stream URL. |
| `src/browse_worker.c` | One worker thread for browse requests. |
| `src/stream_player.c` | One worker thread for HTTP → dr_mp3 → `morpthis_audio_write`. |
| `src/settings.c` | Reading and writing `settings.json`. |
| `src/vendor/` | The player's `http.c`/`tls.c` (adapted to the app), `json.c`, `md5.c` and `dr_mp3.h`. |

Both workers post their results with `morpthis_app_post()`; only the GUI thread
touches widgets. `morpthis_app_shutdown()` stops and joins both threads before
the player unloads the shared object, including when USB mass storage closes
the app to share the card.

The player opens at its usual PCM depth: `morpthis_audio_open(..., 0)` selects
the deep frame-based sizing (8 x 4096 frames, about 750 ms) regardless of the
local playback `[audio] buffer_time_ms` setting. That depth is what rides out a
network stall without an underrun; the player drops it on stop, so leaving the
app is not delayed by it. Explicit time-based requests (the games' short
game buffer) are aligned to the AIC's DMA and fall back to the frame-based
depth when the driver refuses them, so a 44.1 kHz track does not fail at open.

The vendored `http.c` adds `http_abort_all()`, which shuts down the app's open
sockets instead of closing them; the owning thread still closes them. That is
what lets the shutdown join a worker that is blocked in connect, a TLS
handshake or a read without waiting out a network timeout. The vendored
`tls.c` reads `tls_verify` and `ca_bundle` from the app's settings instead of
the player's `device_config.ini`. On an OpenSSL 3 host it sets
`SSL_OP_IGNORE_UNEXPECTED_EOF`, so a server that closes without close_notify
ends the stream cleanly as it already does on the device's OpenSSL 1.1.1.
Strict verification is still the default.

## Build and test

```sh
make -C apps/subsonic MORPTHIS_MODE=host
make -C apps/subsonic MORPTHIS_MODE=host test
make -C apps/subsonic MORPTHIS_MODE=host test-integration
make package                     # device toolchain + mksquashfs
```

`test` is the SDK host smoke test: it opens the settings screen and leaves.
`test-integration` starts a Subsonic-compatible server on `127.0.0.1`, serves
a valid silent MP3 over HTTP and, when `openssl` is available, over HTTPS with
a self-signed certificate in relaxed mode. It exercises the real client, the
JSON parser, dr_mp3 and the PCM writes, and stops a stream part-way to check
that cancellation and teardown complete. It is not part of `make test` because
it needs a listening loopback.

`make package` writes `dist/subsonic.pkgmerp`; copy it to
`Apps/pkgmerps/subsonic.pkgmerp`. The writable folder, including
`settings.json`, is `merp_data/subsonic/` and is not part of the image.

## Licence

The app is distributed under the repository's GPLv3 licence. The vendored
`http.c`, `tls.c`, `json.c` and `md5.c` come from the player, which is also
GPLv3. `dr_mp3.h` is public domain / MIT-0.
