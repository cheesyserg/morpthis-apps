# Subsonic

Streaming from a Subsonic-compatible server, packaged as an app.

This is the test the in-process app model needed: the player's built-in
Subsonic client, rebuilt outside the player and loaded from
`Apps/pkgmerps/subsonic.pkgmerp`. It browses artists, albums, playlists, genres,
starred, random and newest, searches the server, keeps its own queue, and plays
MP3 or lossless FLAC through the SDK's raw PCM output. It does its own HTTP, TLS
and decoding; the player supplies only the display, input, files and DAC.

The player no longer has a built-in Subsonic page, client or RAM streaming
bridge. This package is the only Subsonic client.

API 1 deliberately has no networking calls. The app carries its own copy of the
player's HTTP/TLS stack as vendored sources. That is the point of the test: if
streaming belongs in an app, the next step is an SDK networking call rather
than every streaming app copying `http.c` and `tls.c`.

API 2 is the version this app targets (`min_player: 2`). It adds what the
client needs: 24/32-bit PCM output, the status-bar playing flag, background
operation, the player's icon/text widgets, and a published track for the
control centre. A 24-bit FLAC is written as left-justified S32, which the
player hands to the built-in DAC as S24, so it is bit-perfect; 16-bit FLAC and
MP3 stay at 16 bits, exactly as the player's own decoder treats them. Pressing
BACK while music is loaded leaves the app in the background instead of closing
it, and the player's status bar shows it is still playing.

## Settings

API 1 has no text-entry widget, so the account lives in a file. On the first
launch the app writes a template to:

```text
<card>/merp_data/subsonic/settings.json
```

The app's data folder survives replacing the package. Edit the file from a PC
in USB mass-storage mode (the player offers to close apps gracefully first),
then start the app again, or use **More → Settings → Reload settings** if you
changed it over ADB while the app was running.

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
| `format` | `mp3` (default), `flac` (server transcodes to lossless FLAC) or `raw` (the original file; only FLAC and MP3 originals are decoded). Use `raw` when the server's FLAC transcoder is set to 16 bits and the library has 24-bit files. |
| `max_bitrate` | 64–320 kbps, used only when `format` is `mp3`. The **Quality** button cycles 256 → 320 → FLAC → original (no transcoding). |
| `tls_verify` | `strict` (default) or `relaxed`. Relaxed accepts a self-signed certificate or an unknown issuer, but still checks the name and the dates. |
| `ca_bundle` | Optional absolute path to a PEM bundle. `<card>/.local/cacert.pem` and `/etc/ssl/cert.pem` are checked when this is empty. |

The quickest route to a working server:

- Turn on transcoding in Gonic/Navidrome/Subsonic; MP3 at 256 kbps is the
  default the app asks for, and FLAC gives lossless playback.
- If the server has a self-signed certificate, set `"tls_verify": "relaxed"`
  or tap **TLS** on the settings screen. Prefer a private CA and `ca_bundle`
  where possible.

## Using it

The first screen is a menu: **Artists**, **Albums**, **Folders**, **Playlists**,
**Genres**, **Starred**, **Random**, **Newest** and **Search**. Tapping an artist
shows albums, an album shows its tracks, and a track starts playing. The bottom
row is **Stop** and **More**; the player's chevron, and the swipe-back, go up
one level.

While a track is loaded, the status line at the top of the library becomes a
mini-player row with the cover, title and artist. Tapping it opens the
now-playing page: a large cover, title and artist, elapsed/total time, and the
player's own previous, play/pause and next glyphs. The play glyph in the
heading row opens the same page. The cover is downloaded once per track into
`merp_data/subsonic/covers/current.jpg`; the page and the mini-player read it
from there.

The track is also published to the player, so the status bar's play glyph and
the control centre's now-playing card follow the app instead of the player's
stopped queue. The card's transport buttons come back to the app, and tapping
the card opens this app's now-playing page.

| Control | Action |
| --- | --- |
| **Stop** | Stops playback; the queue is kept. |
| **More** | Opens the menu: **Now playing**, **Queue**, **Star current**, **Settings**, **Back**. |
| PLAY/PAUSE | Pauses and resumes the current stream. |
| NEXT / PREV | Moves through the play queue, not the list on screen. |
| BACK | Goes up one level in the library, from the now-playing page back to it, and back out of the app's other pages. At the root it leaves the app, or keeps it playing in the background when a track is loaded. |
| Volume | Still the player's volume, not the app's. |

Tapping a track queues every track in the list being looked at, starting with
the one tapped. Browsing another album afterwards does not change what NEXT
means; the queue survives until another track is tapped or **Queue → Clear**.

**Search** opens a letter keyboard (A–Z, space, delete). **GO** shows matching
songs, then albums, then artists. **Star current** stars or unstars the track
that is playing; starred tracks are marked with a leading `*` in lists.

The settings screen has **TLS: strict/relaxed**, **Quality: 256/320 kbps, FLAC
or original**, **Reload settings** and **Back**. The first two write
`settings.json`; they do not restart the current track, so the next stream uses
the new setting. **Original** asks the server for the file itself, with no
transcoding; it plays MP3 and FLAC originals and reports a clear error for any
other codec.

When a track ends, the app plays the next track in the queue. The start and the
end of a listen of at least thirty seconds are reported to the server with
`scrobble`, for servers that bridge to Last.fm.

## Limits

This is a working client with a deliberately small footprint, not a complete
Subsonic replacement:

- MP3 and FLAC. Opus is not decoded; choose MP3 or FLAC on the server. With
  **Original** (no transcoding), an Opus, AAC/M4A, Ogg, WAV or other original
  file is refused when it is tapped, with an error naming the format; choose
  MP3 or FLAC transcoding for those. Hi-res is whatever the server sends: a
  24-bit FLAC is played at 32-bit output, but a transcoder configured for
  16-bit FLAC will send 16 bits.
- The first 200 entries of each view. There is no "load more" row yet.
- Cover art only for the current track, and only on the now-playing page and
  the mini-player. Library rows follow the player's library layout (title,
  artist/detail, chevron) without thumbnails, because downloading a cover for
  every row of a 200-entry list would cost more RAM and time than it is worth
  on this device.
- No seeking or gapless playback. Playlists can be browsed and played, but not
  created or edited from the device.
- One stream at a time. Starting another track stops the first.
- No look-ahead buffer: the decoder reads from the socket directly. A link
  that stalls can leave an audible gap. The player's built-in client used two
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
  releases its caches before the app loads. The decoder and the JSON response
  are the app's main allocations; FLAC adds dr_flac's state (tens of KB) and a
  PCM buffer that is 16-bit for MP3 or 32-bit for a 24-bit FLAC.

## How it is put together

| File | Role |
| --- | --- |
| `src/main.c` | Screens, browsing state, queue, star, search and settings toggles. |
| `src/subsonic_client.c` | REST client: token auth, browse endpoints, stream URL, star, scrobble. |
| `src/browse_worker.c` | One worker thread for browse requests. |
| `src/cover_worker.c` | One worker thread for the current track's cover art. |
| `src/stream_player.c` | One worker thread for HTTP → dr_mp3/dr_flac → `morpthis_audio_write_pcm`. |
| `src/settings.c` | Reading and writing `settings.json`. |
| `src/vendor/` | The player's `http.c`/`tls.c` (adapted to the app), `json.c`, `md5.c`, `dr_mp3.h` and `dr_flac.h`. |

The workers post their results with `morpthis_app_post()`; only the GUI thread
touches widgets. `morpthis_app_shutdown()` stops and joins them before the
player unloads the shared object, including when USB mass storage closes the
app to share the card.

The player opens at its usual PCM depth: `morpthis_audio_open_ex(..., 0)`
selects the deep frame-based sizing (8 x 4096 frames, about 750 ms) regardless
of the local playback `[audio] buffer_time_ms` setting. That depth is what rides
out a network stall without an underrun; the player drops it on stop, so leaving
the app is not delayed by it. Explicit time-based requests (the games' short
game buffer) are aligned to the AIC's DMA and fall back to the frame-based
depth when the driver refuses them, so a 44.1 kHz track does not fail at open.
The depth is chosen per track from the decoder: 32 bits when FLAC reports more
than 16 bits per sample, 16 bits otherwise. If the output refuses S32 the app
retries at 16 and says so in the log, which is the same fallback local playback
uses.

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

`test` is the SDK host smoke test: it opens the app menu and the settings screen
and leaves. `test-integration` starts a Subsonic-compatible server on
`127.0.0.1`, serves a valid silent MP3 and a real 24-bit FLAC stream over HTTP
and, when `openssl` is available, over HTTPS with a self-signed certificate in
relaxed mode. It exercises the real client, the JSON parser, dr_mp3, dr_flac
and the PCM writes, checks the exact 327,680-frame lossless decode at 32-bit
output, and stops a stream part-way to check that cancellation and teardown
complete. It is not part of `make test` because it needs a listening loopback.

`make package` writes `dist/subsonic.pkgmerp`; copy it to
`Apps/pkgmerps/subsonic.pkgmerp`. The writable folder, including
`settings.json`, is `merp_data/subsonic/` and is not part of the image.

## Licence

The app is distributed under the repository's GPLv3 licence. The vendored
`http.c`, `tls.c`, `json.c` and `md5.c` come from the player, which is also
GPLv3. `dr_mp3.h` is public domain / MIT-0.
