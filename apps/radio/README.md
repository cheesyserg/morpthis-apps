# Radio

Internet radio on radio-browser.info, with the directory, the transport and
the UI all in the app. The player provides only the SDK: raw PCM, the published
track, the power holds and the player's widgets.

## What it does

- **Browse:** popular stations, genres, countries and languages.
- **Search** by station name.
- **Favourites** and **recently played**, kept in
  `Apps/merp_data/radio/stations.txt` on the card.
- **Custom stations:** a name and an `http://` or `https://` address, typed on
  the player's keyboard.
- **Playback:** MP3 and ADTS AAC, direct or through an HLS manifest. ICY titles
  are shown on the Now Playing page and in the control centre.
- **Background play:** leaving the app with a station loaded keeps it on air;
  the control centre and the status bar follow it. The player's power hold is
  `AWAKE | NETWORK` while a station is playing.
- **Pause** closes the stream, because a live stream has no useful buffer to
  resume from; **play** reconnects at the live point.

## The shared layer

`../../lib/streaming/` holds the parts another streaming app will want too:

| Piece | Source |
| --- | --- |
| HTTP/1.1 and TLS | Subsonic's vendored `http.c`/`tls.c` |
| HLS manifest and segment unwrapping | the player's former `hls.c` |
| MP3 | dr_mp3 |
| ADTS AAC | the player's `aacdec.c` (libfdk-aac opened at run time) |

The app copies these into its own package; nothing is linked from the player
except the public SDK.

## Limits worth knowing

- **Live seeking** is not offered: these are live streams, not files.
- **The directory is shown 100 stations at a time.** Search is the way to
  reach a station that is not in the first page of a large country or genre.
- **Station logos are not downloaded.** The Now Playing page uses the app's
  own icon; the directory's favicons are available for a later pass.
- **AAC** depends on the device's `/usr/lib/libfdk-aac.so.2` (the same library
  bluealsa uses). Without it, MP3 stations still play and an AAC station says
  why it cannot.
- **The equalizer** is not applied to app PCM: the SDK's raw output bypasses
  the player's decode loop where the EQ lives. Local playback is unaffected.
- **fMP4 HLS** is not supported; the segment unwrapper says so.

## Build

```sh
make -C apps/radio MORPTHIS_MODE=host
make -C apps/radio MORPTHIS_MODE=host test
make -C apps/radio MORPTHIS_MODE=device
python3 tools/validate-manifest.py apps/radio
make package
```

The package is `dist/radio.pkgmerp`; copy it to
`Apps/pkgmerps/radio.pkgmerp` on the card.
