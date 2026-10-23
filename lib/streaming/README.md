# lib/streaming

Sources shared by the network streaming apps. There is no library to install:
each app adds the files it needs to `APP_SRCS`, so they are compiled into its
own `.pkgmerp`. That keeps every package self-contained on a card the player can
read but not write.

| File | What it is | Where it came from |
| --- | --- | --- |
| `http.c/.h` | HTTP/1.1 client, ICY metadata, streaming reads, redirects | Subsonic's vendor copy |
| `tls.c/.h` | OpenSSL opened at run time, CA bundle search, strict/relaxed verification | Subsonic's vendor copy |
| `json.c/.h` | token-tree parser for nested service APIs | Subsonic's vendor copy; unused by Radio, kept for the next service |
| `hls.c/.h` | HLS manifests, variants and MPEG-TS unwrapping | the player's former `src/system/net/hls.c` |
| `decoders/dr_mp3*` | MP3 | dr_mp3 0.7.4 (public domain / MIT-0) |
| `decoders/aacdec.*` | ADTS AAC | the player's `src/system/decode/aacdec.c`, `dlopen`ing the device's libfdk-aac |

The player must not grow a second streaming stack for an app. These files run
inside the app's thread and reach the device only through the public SDK's PCM
output, power holds and track publication.

## Adding another app

1. Add the sources you need to `APP_SRCS`, for example:
   ```make
   APP_SRCS := src/main.c \
       ../../lib/streaming/http.c ../../lib/streaming/tls.c \
       ../../lib/streaming/decoders/dr_mp3_impl.c
   APP_INCLUDES := -I../../lib/streaming -I../../lib/streaming/decoders
   APP_CFLAGS := -D_GNU_SOURCE
   ```
2. Do not link against the player's internals. The app is a plugin and only
   the `morpthis_*` symbols are exported to it.
3. Device builds compile with `-fvisibility=hidden`, which is what keeps the
   copies of `http_get`, `aacdec_*` and friends local to the app even though
   the player executable exports similarly named symbols.

## Keeping the copies fresh

`http.c` and `tls.c` are deliberately close to Subsonic's copies. If a bug is
fixed in one, fix it in both until the two apps are changed to share a single
source file; the copies exist because the app build does not have a shared
runtime to link against.
