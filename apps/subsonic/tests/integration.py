#!/usr/bin/env python3
"""Loopback test for the Subsonic app's network and playback path.

    python3 tests/integration.py build/host/test_stream

Starts a tiny Subsonic-compatible HTTP server on 127.0.0.1, runs the C test
binary against it, and checks its exit status. No external network and no
server is needed; the "MP3" is a run of valid silent MPEG-1 Layer III frames and
the FLAC stream is a real 80-block verbatim stream.
"""

from __future__ import annotations

import http.server
import json
import os
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import threading

# MPEG-1 Layer III, 128 kbps, 44.1 kHz, stereo. 417 bytes is the frame size
# for those parameters; the payload is zero, which decodes as silence. This is
# a real frame a decoder can parse, not random bytes.
FRAME = bytes([0xFF, 0xFB, 0x90, 0x00]) + bytes(413)
STREAM_FRAMES = 600
STREAM = FRAME * STREAM_FRAMES

# Every /rest/stream.view path the test binary asked for. The seek test checks
# that the server was really told to start three seconds in.
REQUEST_LOCK = threading.Lock()
STREAM_REQUESTS: list[str] = []


def crc(data: bytes, bits: int, polynomial: int) -> int:
    value = 0
    mask = (1 << bits) - 1
    for byte in data:
        value ^= byte << (bits - 8)
        for _ in range(8):
            value = ((value << 1) ^ (polynomial if value & (1 << (bits - 1)) else 0)) & mask
    return value


def make_flac() -> bytes:
    """80 blocks x 4096 samples, 44.1 kHz stereo 24-bit, verbatim frames.

    A real STREAMINFO and real frame CRCs, so dr_flac decodes it as a normal
    lossless stream. The exact PCM frame count is 80 * 4096. Twenty-four bits
    is the point of the fixture: the app must ask the output for 32-bit
    left-justified samples rather than truncate it to 16.
    """
    count, block = 80, 4096
    frames = []
    for i in range(count):
        # 0x1C: independent stereo, 24 bits per sample.
        header = b"\xff\xf8\x79\x1c" + bytes([i]) + struct.pack(">H", block - 1)
        header += bytes([crc(header, 8, 0x07)])
        frame = header + (b"\x02" + bytes(block * 3)) * 2
        frames.append(frame + struct.pack(">H", crc(frame, 16, 0x8005)))
    size = len(frames[0])
    info = struct.pack(">HH", block, block) + size.to_bytes(3, "big") * 2
    info += ((44100 << 44) | (1 << 41) | (23 << 36) | (count * block)).to_bytes(8, "big")
    info += bytes(16)  # unspecified PCM MD5, legal for a live encoder
    metadata = b"\x80" + b"\x00\x00\x22" + info
    return b"fLaC" + metadata + b"".join(frames)


FLAC_STREAM = make_flac()
FLAC_FRAMES = 80 * 4096

ALBUM = {
    "subsonic-response": {
        "status": "ok",
        "version": "1.16.1",
        "album": {
            "id": "al1",
            "name": "Test Album",
            "artist": "Tester",
            "song": [
                {
                    "id": "tr1",
                    "title": "First Track",
                    "artist": "Tester",
                    "album": "Test Album",
                    "duration": 6,
                    "track": 1,
                    "suffix": "mp3",
                    "isDir": False,
                    "starred": "2024-01-01T00:00:00Z",
                },
                {
                    "id": "tr2",
                    "title": "Second Track",
                    "artist": "Tester",
                    "album": "Test Album",
                    "duration": 6,
                    "track": 2,
                    "suffix": "mp3",
                    "isDir": False,
                },
            ],
        },
    }
}

PLAYLISTS = {
    "subsonic-response": {
        "status": "ok",
        "version": "1.16.1",
        "playlists": {
            "playlist": [
                {"id": "pl1", "name": "Road Trip", "songCount": 2},
            ]
        },
    }
}

STARRED = {
    "subsonic-response": {
        "status": "ok",
        "version": "1.16.1",
        "starred2": {
            "song": [
                {"id": "tr1", "title": "First Track", "artist": "Tester", "starred": "2024-01-01T00:00:00Z"},
            ],
            "album": [
                {"id": "al1", "name": "Test Album", "artist": "Tester", "starred": "2024-01-01T00:00:00Z"},
            ],
        },
    }
}

GENRES = {
    "subsonic-response": {
        "status": "ok",
        "version": "1.16.1",
        "genres": {
            "genre": [
                {"value": "Rock", "songCount": 5},
            ]
        },
    }
}

SEARCH = {
    "subsonic-response": {
        "status": "ok",
        "version": "1.16.1",
        "searchResult3": {
            "song": [
                {"id": "tr1", "title": "First Track", "artist": "Tester", "suffix": "mp3"},
            ],
        },
    }
}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, format, *args):  # noqa: A002 - the base signature
        pass

    def send_body(self, body: bytes, content_type: str) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802 - the base name
        if self.path.startswith("/rest/stream.view"):
            with REQUEST_LOCK:
                STREAM_REQUESTS.append(self.path)
        if self.path.startswith("/rest/getAlbum.view"):
            self.send_body(json.dumps(ALBUM).encode(), "application/json")
        elif self.path.startswith("/rest/getPlaylists.view"):
            self.send_body(json.dumps(PLAYLISTS).encode(), "application/json")
        elif self.path.startswith("/rest/getStarred2.view"):
            self.send_body(json.dumps(STARRED).encode(), "application/json")
        elif self.path.startswith("/rest/getGenres.view"):
            self.send_body(json.dumps(GENRES).encode(), "application/json")
        elif self.path.startswith("/rest/search3.view"):
            self.send_body(json.dumps(SEARCH).encode(), "application/json")
        elif self.path.startswith("/rest/stream.view"):
            if "format=flac" in self.path:
                self.send_body(FLAC_STREAM, "audio/flac")
            else:
                self.send_body(STREAM, "audio/mpeg")
        else:
            self.send_body(
                b'{"subsonic-response":{"status":"ok","version":"1.16.1"}}',
                "application/json",
            )


def run_case(binary: list[str], scheme: str, tls_mode: str, certfile: str | None = None,
             keyfile: str | None = None) -> tuple[int, list[str]]:
    with REQUEST_LOCK:
        STREAM_REQUESTS.clear()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    if scheme == "https":
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile, keyfile)
        server.socket = context.wrap_socket(server.socket, server_side=True)

    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    env = dict(os.environ)
    env["SUBSONIC_TEST_PORT"] = str(server.server_address[1])
    env["SUBSONIC_TEST_SCHEME"] = scheme
    env["SUBSONIC_TEST_TLS"] = tls_mode
    print(f"integration: {scheme}, TLS {tls_mode}")
    try:
        result = subprocess.run(binary, env=env, timeout=30)
    except subprocess.TimeoutExpired:
        print("integration: the test binary did not finish in 30 seconds", file=sys.stderr)
        return 1
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)

    if result.returncode != 0:
        print(f"integration: test binary exited {result.returncode}", file=sys.stderr)
    with REQUEST_LOCK:
        requests = list(STREAM_REQUESTS)
    return result.returncode, requests


def make_self_signed(directory: str) -> tuple[str, str] | None:
    """A certificate for 127.0.0.1, valid long enough for one test run."""
    cert = os.path.join(directory, "cert.pem")
    key = os.path.join(directory, "key.pem")
    command = [
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256",
        "-days", "2", "-keyout", key, "-out", cert, "-subj", "/CN=127.0.0.1",
        "-addext", "subjectAltName=IP:127.0.0.1",
    ]
    result = subprocess.run(command, capture_output=True)
    if result.returncode != 0:
        return None
    return cert, key


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: integration.py <test-binary>", file=sys.stderr)
        return 2

    status, requests = run_case(argv[1:], "http", "strict")
    if status != 0:
        return 1
    # The C test seeks to 3 s; the server must have been asked to start there
    # rather than at the beginning of the file.
    if not any("timeOffset=3" in path for path in requests):
        print("integration: the seek did not reach the server as timeOffset=3", file=sys.stderr)
        print(f"integration: stream requests were {requests}", file=sys.stderr)
        return 1

    if shutil.which("openssl") is None:
        print("integration: openssl not found; HTTPS case skipped")
        print("integration: ok")
        return 0

    with tempfile.TemporaryDirectory(prefix="subsonic-cert-") as directory:
        pair = make_self_signed(directory)
        if pair is None:
            print("integration: could not make a certificate; HTTPS case skipped")
            print("integration: ok")
            return 0
        cert, key = pair
        # The certificate is self-signed, so relaxed is the mode a user with a
        # home server turns on. Strict is the default and is tested by the HTTP
        # case only: the point here is that the app can reach this server.
        status, requests = run_case(argv[1:], "https", "relaxed", cert, key)
        if status != 0:
            return 1
        if not any("timeOffset=3" in path for path in requests):
            print("integration: the HTTPS seek did not reach the server", file=sys.stderr)
            return 1

    print("integration: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
