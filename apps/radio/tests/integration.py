#!/usr/bin/env python3
"""Loopback test for the radio app.

    python3 tests/integration.py build/host/test_radio

Serves a fake radio-browser directory, a dribbled MP3 stream and an HLS
manifest whose segments are bare MP3. No external network is used.
"""

from __future__ import annotations

import http.server
import json
import os
import subprocess
import sys
import threading
import time

# MPEG-1 Layer III, 128 kbps, 44.1 kHz, stereo: 417 bytes per frame, zero
# payload (silence). A real frame the decoder can parse.
FRAME = bytes([0xFF, 0xFB, 0x90, 0x00]) + bytes(413)
STREAM = FRAME * 300
SEGMENT = FRAME * 100

GENRES = [
    {"name": "rock", "stationcount": 120},
    {"name": "jazz", "stationcount": 45},
]

COUNTRIES = [
    {"name": "Italy", "iso_3166_1": "IT", "stationcount": 10},
]

LANGUAGES = [
    {"name": "italian", "stationcount": 8},
]

STATIONS = [
    {
        "stationuuid": "u1",
        "name": "Test One",
        "url": "http://example.invalid/raw",
        "url_resolved": "http://example.invalid/one",
        "countrycode": "IT",
        "codec": "MP3",
        "bitrate": 128,
    },
    {
        "stationuuid": "u2",
        "name": "Test Two",
        "url": "http://example.invalid/two",
        "url_resolved": "",
        "countrycode": "DE",
        "codec": "AAC",
        "bitrate": 64,
    },
]

HLS_MANIFEST = (
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "#EXT-X-TARGETDURATION:1\n"
    "#EXT-X-MEDIA-SEQUENCE:1\n"
    "#EXTINF:1.0,\n"
    "seg1.mp3\n"
    "#EXTINF:1.0,\n"
    "seg2.mp3\n"
    "#EXT-X-ENDLIST\n"
).encode()

REQUEST_LOCK = threading.Lock()
REQUESTS: list[str] = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, format, *args):  # noqa: A002 - base signature
        pass

    def send_body(self, body: bytes, content_type: str, dribble: bool = False) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            if not dribble:
                self.wfile.write(body)
                return
            step = 700
            for start in range(0, len(body), step):
                self.wfile.write(body[start:start + step])
                self.wfile.flush()
                time.sleep(0.001)
        except (BrokenPipeError, ConnectionResetError):
            # The player stopped the stream; that is a test success, not a
            # server error.
            pass

    def send_icy(self) -> None:
        """An endless-looking MP3 with one ICY title block after 8 KiB."""
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("icy-metaint", "8192")
        self.send_header("icy-name", "Test ICY")
        self.end_headers()

        meta = b"StreamTitle='Test Song';"
        pad = (16 - len(meta) % 16) % 16
        block = bytes([(len(meta) + pad) // 16]) + meta + bytes(pad)
        try:
            self.wfile.write(STREAM[:8192])
            self.wfile.write(block)
            self.wfile.write(STREAM[8192:])
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):  # noqa: N802 - base name
        with REQUEST_LOCK:
            REQUESTS.append(self.path)

        path = self.path
        if path.startswith("/json/tags"):
            self.send_body(json.dumps(GENRES).encode(), "application/json")
        elif path.startswith("/json/countries"):
            self.send_body(json.dumps(COUNTRIES).encode(), "application/json")
        elif path.startswith("/json/languages"):
            self.send_body(json.dumps(LANGUAGES).encode(), "application/json")
        elif path.startswith("/json/stations/topclick"):
            self.send_body(json.dumps(STATIONS).encode(), "application/json")
        elif path.startswith("/json/stations/search"):
            if "name=test" in path:
                self.send_body(json.dumps(STATIONS).encode(), "application/json")
            elif "country=Italy" in path:
                self.send_body(json.dumps(STATIONS[:1]).encode(), "application/json")
            elif "tag=rock" in path:
                self.send_body(json.dumps(STATIONS[:1]).encode(), "application/json")
            else:
                self.send_body(b"[]", "application/json")
        elif path.startswith("/live.m3u8"):
            self.send_body(HLS_MANIFEST, "application/vnd.apple.mpegurl")
        elif path.startswith("/seg"):
            self.send_body(SEGMENT, "audio/mpeg")
        elif path.startswith("/stream.mp3"):
            # Small writes make recv() return short reads, as a real network
            # does and as the ID3/ring code must cope with.
            self.send_body(STREAM, "audio/mpeg", dribble=True)
        elif path.startswith("/icy.mp3"):
            self.send_icy()
        else:
            self.send_body(b"[]", "application/json")


def run(binary: list[str]) -> int:
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    env = dict(os.environ)
    env["RADIO_TEST_PORT"] = str(server.server_address[1])
    try:
        result = subprocess.run(binary, env=env, timeout=45)
    except subprocess.TimeoutExpired:
        print("integration: the test binary did not finish in 45 seconds", file=sys.stderr)
        return 1
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)

    with REQUEST_LOCK:
        requests = list(REQUESTS)

    if not any("country=Italy&countryExact=true" in path for path in requests):
        print("integration: the country browse did not use the exact-match parameter", file=sys.stderr)
        print(f"integration: requests were {requests}", file=sys.stderr)
        return 1
    if not any(path.startswith("/live.m3u8") for path in requests):
        print("integration: the HLS manifest was never requested", file=sys.stderr)
        return 1
    if not any(path.startswith("/seg") for path in requests):
        print("integration: the HLS segments were never requested", file=sys.stderr)
        return 1

    return result.returncode


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: integration.py <test-binary>", file=sys.stderr)
        return 2
    status = run(argv[1:])
    if status != 0:
        print(f"integration: test binary exited {status}", file=sys.stderr)
        return 1
    print("integration: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
