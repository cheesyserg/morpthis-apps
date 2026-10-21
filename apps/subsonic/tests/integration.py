#!/usr/bin/env python3
"""Loopback test for the Subsonic app's network and playback path.

    python3 tests/integration.py build/host/test_stream

Starts a tiny Subsonic-compatible HTTP server on 127.0.0.1, runs the C test
binary against it, and checks its exit status. No external network and no
server is needed; the "MP3" is a run of valid silent MPEG-1 Layer III frames.
"""

from __future__ import annotations

import http.server
import json
import os
import shutil
import ssl
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
        if self.path.startswith("/rest/getAlbum.view"):
            self.send_body(json.dumps(ALBUM).encode(), "application/json")
        elif self.path.startswith("/rest/stream.view"):
            self.send_body(STREAM, "audio/mpeg")
        else:
            self.send_body(
                b'{"subsonic-response":{"status":"ok","version":"1.16.1"}}',
                "application/json",
            )


def run_case(binary: list[str], scheme: str, tls_mode: str, certfile: str | None = None,
             keyfile: str | None = None) -> int:
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
    return result.returncode


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

    if run_case(argv[1:], "http", "strict") != 0:
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
        if run_case(argv[1:], "https", "relaxed", cert, key) != 0:
            return 1

    print("integration: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
