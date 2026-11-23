#!/usr/bin/env python3
"""Leftover scaled PNG must not stay in the viewer web directory.

pngToGray.php is what the viewer runs for every instance it displays.
viewer.php loads the first slice with pngToGray.php?instance=<uuid>
(getGrayFromPNG). The same URL is requested by bgLoadOdd.js,
bgLoadEven.js, bgLoadOddTwo.js, and bgLoadEvenTwo.js.

When the PNG from Orthanc /instances/<id>/image-uint16 is wider or
taller than 1000 pixels, the helper writes that body to the fixed
relative path image.png in the script directory and never removes it.
That directory is the web root (Apache/PHP, per the viewer README).
These tests request the helper the way the viewer does, then request
image.png with no instance id, the way anyone who can reach the
application directory can.

They fail while that file, or any other copy of the fetched PNG, is
left in the web root. They pass once a request no longer leaves the
fetched bytes there. Scaling from memory, or a temporary file outside
this directory that is gone when the request finishes, satisfies them.

Run from the repository root:

    python3 Applications/Php/SimpleViewerEmsy/tests/test_leftover_scaled_png.py

Requires the php CLI on PATH. The helper hardcodes Orthanc at
http://localhost:8042, so this process must be able to bind that port.
"""

import os
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPO_PHP = Path(__file__).resolve().parents[1] / "pngToGray.php"
ORTHANC_PORT = 8042
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def orthanc_uint16_png(width, height, marker):
    """PNG whose IHDR width and height are the image dimensions.

    Orthanc 0.8.3 (the server named in viewer.php) answers
    /instances/{id}/image-uint16 with a PNG. PngWriter::Compress passes
    the decoded image width and height to png_set_IHDR. libpng 1.6.43
    png_write_IHDR stores those values as big-endian uint32s immediately
    after the IHDR tag, which is the layout pngToGray.php reads. A tEXt
    chunk carries marker so the exact response can be recognized on disk.
    """
    if not isinstance(marker, bytes):
        raise TypeError("marker must be bytes")

    def chunk(tag, data):
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    # 16-bit grayscale: bit depth 16, color type 0, compression 0, filter 0, interlace 0.
    ihdr = struct.pack(">IIBBBBB", width, height, 16, 0, 0, 0, 0)
    text = b"Comment\x00" + marker
    # The helper writes the file before it finishes decoding, so the IDAT
    # does not have to expand to width*height.
    idat = zlib.compress(b"\x00" + marker[:2].ljust(2, b"\x00"))
    return (
        PNG_SIGNATURE
        + chunk(b"IHDR", ihdr)
        + chunk(b"tEXt", text)
        + chunk(b"IDAT", idat)
        + chunk(b"IEND", b"")
    )


class QuietHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return


class OrthancStub(threading.Thread):
    """GET /instances/<id>/image-uint16 returns that instance's PNG bytes."""

    def __init__(self):
        super().__init__(daemon=True)
        self.images = {}
        self.hits = []
        self._lock = threading.Lock()
        self._ready = threading.Event()
        self._server = None
        self.error = None

    def set_image(self, instance_id, png_bytes):
        with self._lock:
            self.images[instance_id] = png_bytes

    def run(self):
        owner = self

        class Handler(QuietHandler):
            def do_GET(self):
                path = self.path.split("?", 1)[0]
                parts = [p for p in path.split("/") if p]
                if (
                    len(parts) == 3
                    and parts[0] == "instances"
                    and parts[2] == "image-uint16"
                ):
                    instance_id = urllib.parse.unquote(parts[1])
                    with owner._lock:
                        body = owner.images.get(instance_id)
                        owner.hits.append(instance_id)
                    if body is None:
                        self.send_response(404)
                        self.end_headers()
                        return
                    self.send_response(200)
                    self.send_header("Content-Type", "image/png")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                self.send_response(404)
                self.end_headers()

        try:
            self._server = ThreadingHTTPServer(("127.0.0.1", ORTHANC_PORT), Handler)
        except OSError as exc:
            self.error = exc
            self._ready.set()
            return
        self._ready.set()
        self._server.serve_forever()

    def start_and_wait(self):
        self.start()
        self._ready.wait(5)
        if self.error is not None:
            raise self.error

    def stop(self):
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def wait_for_port(port):
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("php server did not accept connections on port %s" % port)


class ViewerWeb(object):
    def __init__(self, docroot):
        self.docroot = docroot
        self.port = free_port()
        self.proc = subprocess.Popen(
            ["php", "-S", "127.0.0.1:%d" % self.port, "-t", docroot],
            cwd=docroot,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        wait_for_port(self.port)

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def get(self, path):
        request = urllib.request.Request(
            "http://127.0.0.1:%d%s" % (self.port, path),
            method="GET",
        )
        try:
            with urllib.request.urlopen(request, timeout=20) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read()


def snapshot(docroot):
    found = {}
    for dirpath, _dirs, names in os.walk(docroot):
        for name in names:
            path = os.path.join(dirpath, name)
            with open(path, "rb") as handle:
                found[os.path.relpath(path, docroot)] = handle.read()
    return found


class LeftoverScaledPngTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("php") is None:
            raise AssertionError("php CLI is required to run pngToGray.php")
        if not REPO_PHP.is_file():
            raise AssertionError("missing %s" % REPO_PHP)
        cls.orthanc = OrthancStub()
        cls.orthanc.start_and_wait()

    @classmethod
    def tearDownClass(cls):
        orthanc = getattr(cls, "orthanc", None)
        if orthanc is not None:
            orthanc.stop()

    def setUp(self):
        self._tmpdir = tempfile.TemporaryDirectory()
        self.docroot = self._tmpdir.name
        shutil.copy(REPO_PHP, os.path.join(self.docroot, "pngToGray.php"))
        self.web = ViewerWeb(self.docroot)

    def tearDown(self):
        self.web.close()
        self._tmpdir.cleanup()

    def open_instance(self, instance_id, width, height):
        marker = ("pixels-" + instance_id).encode("ascii")
        png = orthanc_uint16_png(width, height, marker)
        self.orthanc.set_image(instance_id, png)
        # Same request the viewer and its four workers issue.
        status, _body = self.web.get("/pngToGray.php?instance=" + instance_id)
        self.assertIn(
            instance_id,
            self.orthanc.hits,
            "helper did not fetch /instances/%s/image-uint16" % instance_id,
        )
        self.assertEqual(status, 200)
        return png

    def assert_pixels_not_stored(self, *pngs):
        after = snapshot(self.docroot)
        self.assertEqual(
            sorted(after),
            ["pngToGray.php"],
            "web root contains more than the helper script: %s" % sorted(after),
        )
        self.assertEqual(after["pngToGray.php"], REPO_PHP.read_bytes())
        status, body = self.web.get("/image.png")
        self.assertNotEqual(
            status,
            200,
            "GET /image.png without an instance id returned a file",
        )
        self.assertFalse(body.startswith(PNG_SIGNATURE))
        for png in pngs:
            self.assertNotEqual(body, png)
            for name, data in after.items():
                self.assertNotEqual(data, png, "%s is the fetched PNG" % name)

    def test_width_1001_second_scale_does_not_leave_png(self):
        png = self.open_instance("wide-1001", 1001, 1)
        self.assert_pixels_not_stored(png)

    def test_height_1001_second_scale_does_not_leave_png(self):
        png = self.open_instance("tall-1001", 1, 1001)
        self.assert_pixels_not_stored(png)

    def test_width_2000_second_scale_does_not_leave_png(self):
        # 2000 is not greater than 2000, so only the second write runs.
        png = self.open_instance("wide-2000", 2000, 1)
        self.assert_pixels_not_stored(png)

    def test_width_2001_first_scale_does_not_leave_png(self):
        png = self.open_instance("wide-2001", 2001, 1)
        self.assert_pixels_not_stored(png)

    def test_height_2001_first_scale_does_not_leave_png(self):
        png = self.open_instance("tall-2001", 1, 2001)
        self.assert_pixels_not_stored(png)

    def test_width_4001_both_scales_do_not_leave_png(self):
        # Above 2000 writes once, then the quartered dimension is still above 1000.
        png = self.open_instance("wide-4001", 4001, 1)
        self.assert_pixels_not_stored(png)

    def test_height_4001_both_scales_do_not_leave_png(self):
        png = self.open_instance("tall-4001", 1, 4001)
        self.assert_pixels_not_stored(png)

    def test_boundary_1000_does_not_write_png(self):
        png = self.open_instance("exact-1000", 1000, 1000)
        self.assert_pixels_not_stored(png)

    def test_second_large_image_does_not_leave_either_png(self):
        first = self.open_instance("series-a", 1500, 8)
        second = self.open_instance("series-b", 1800, 8)
        self.assert_pixels_not_stored(first, second)

    def test_smaller_image_afterwards_does_not_keep_the_large_png(self):
        large = self.open_instance("kept-large", 1200, 4)
        small = self.open_instance("later-small", 64, 64)
        self.assert_pixels_not_stored(large, small)


if __name__ == "__main__":
    unittest.main(verbosity=2)
