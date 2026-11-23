#!/usr/bin/env python3
"""Failing tests for the fixed scanned.png left by DicomizeImage uploads.

createDicom.php re-encodes the posted image and writes it to the relative
path scanned.png. These tests drive that script the way the upload form
does and require the document not to remain in the web directory.

They fail while the predictable file is still written and left behind.
They pass once an upload still reaches Orthanc and the re-encoded PNG is
gone from the application directory before the request finishes.

Run (PHP 7.4 CLI and CGI, both with gd and curl):

    PHP_BINARY=/usr/local/php-7.4/bin/php \\
    PHP_CGI=/usr/local/php-7.4/bin/php-cgi \\
    python3 tests/test_dicomize_image_fixed_filename.py

PHP 8 throws on the unqualified Ymd constant in createDicom.php before
data_uri() runs, so it does not execute this upload path. PHP 7.4.33 is
the runtime these tests use: an unqualified constant becomes the string
of the same name, date(Ymd) succeeds, and the write runs.

Not covered here, same class of fixed filename but a different operation:
Applications/Php/SimpleViewerEmsy/pngToGray.php (image.png) and
Applications/Php/Weasis/loadWeasisX.php (weasisX.xml). GIF, BMP, WBMP,
and WEBP uploads hit the same imagepng() call as the PNG and JPEG cases
below and do not have separate tests.
"""

import base64
import os
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
APP_SRC = REPO / "Applications" / "Php" / "DicomizeImage"
ORTHANC_PORT = 8042
PIXELDATA_RE = re.compile(
    br'"PixelData":"data:image/png;base64,([A-Za-z0-9+/=]+)"'
)
# Response shape consumed by createDicom.php: explode on '"' and take
# index 7, then strip a leading /instances/.
CREATE_DICOM_RESPONSE = b'"a""b""c""/instances/inst-fixed"'


def _php_from_env(env_name, fallbacks):
    candidates = []
    if os.environ.get(env_name):
        candidates.append(os.environ[env_name])
    candidates.extend(fallbacks)
    for candidate in candidates:
        if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def php_binary():
    return _php_from_env(
        "PHP_BINARY",
        ["/usr/local/php-7.4/bin/php", "/usr/bin/php"],
    )


def php_cgi_binary():
    return _php_from_env(
        "PHP_CGI",
        ["/usr/local/php-7.4/bin/php-cgi", "/usr/bin/php-cgi"],
    )


def php_version(binary):
    out = subprocess.check_output([binary, "-r", "echo PHP_VERSION;"], text=True)
    return out.strip()


def solid_png(width, height, rgb):
    def chunk(tag, data):
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw))
        + chunk(b"IEND", b"")
    )


def solid_jpeg(php, rgb):
    """A solid JPEG produced by the same GD build that decodes the upload."""
    red, green, blue = rgb
    script = (
        "$im=imagecreatetruecolor(8,8);"
        f"$c=imagecolorallocate($im,{red},{green},{blue});"
        "imagefilledrectangle($im,0,0,7,7,$c);"
        "imagejpeg($im,'php://stdout',100);"
    )
    return subprocess.check_output([php, "-r", script])


def png_rgb_at(php, data, x, y):
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        return None
    fd, name = tempfile.mkstemp(suffix=".png")
    os.close(fd)
    try:
        Path(name).write_bytes(data)
        script = (
            "$im=imagecreatefromstring(file_get_contents($argv[1]));"
            "if($im===false){exit(2);}"
            f"$c=imagecolorat($im,{int(x)},{int(y)});"
            'echo (($c>>16)&255),",",(($c>>8)&255),",",($c&255);'
        )
        out = subprocess.check_output([php, "-r", script, name], text=True)
        return tuple(int(part) for part in out.strip().split(","))
    except subprocess.CalledProcessError:
        return None
    finally:
        os.unlink(name)


def pixeldata_rgb(php, body):
    match = PIXELDATA_RE.search(body)
    if not match:
        return None
    return png_rgb_at(php, base64.b64decode(match.group(1)), 0, 0)


def multipart_body(image_bytes, filename, content_type, patient_name, patient_id):
    boundary = "----DicomizeTestBoundary"
    parts = []
    for name, value in (("PatientName", patient_name), ("PatientID", patient_id)):
        parts.append(
            (
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="{name}"\r\n\r\n'
                f"{value}\r\n"
            ).encode()
        )
    parts.append(
        (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
            f"Content-Type: {content_type}\r\n\r\n"
        ).encode()
        + image_bytes
        + b"\r\n"
    )
    parts.append(f"--{boundary}--\r\n".encode())
    body = b"".join(parts)
    return boundary, body


class OrthancStandIn(threading.Thread):
    """Records the create-dicom body this script actually posts.

    The body layout is the one createCurlPostFieldsData() builds:
    "tag":"value" pairs. The reply is the quote-separated text
    createDicom.php splits to find the instance id.
    """

    def __init__(self):
        super().__init__(daemon=True)
        self.posts = []
        self._httpd = ThreadingHTTPServer(("127.0.0.1", ORTHANC_PORT), self._handler())
        self._httpd.posts = self.posts

    def _handler(self):
        parent = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                parent.posts.append((self.path, body))
                if self.path == "/tools/create-dicom":
                    payload = CREATE_DICOM_RESPONSE
                else:
                    payload = b"{}"
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_DELETE(self):
                parent.posts.append((self.path, b""))
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, fmt, *args):
                return

        return Handler

    def run(self):
        self._httpd.serve_forever()

    def stop(self):
        self._httpd.shutdown()
        self._httpd.server_close()


class DicomizeFixedFilenameTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.php = php_binary()
        cls.php_cgi = php_cgi_binary()
        if not cls.php or not cls.php_cgi:
            raise RuntimeError(
                "PHP CLI and CGI with gd and curl are required. Set PHP_BINARY "
                "and PHP_CGI. PHP 7.4 executes this upload path; PHP 8 aborts "
                "on the unqualified Ymd constant before the image is written."
            )
        cls.version = php_version(cls.php)
        try:
            cls.orthanc = OrthancStandIn()
        except OSError as exc:
            raise RuntimeError(f"cannot bind 127.0.0.1:{ORTHANC_PORT}: {exc}") from exc
        cls.orthanc.start()
        time.sleep(0.1)

    @classmethod
    def tearDownClass(cls):
        orthanc = getattr(cls, "orthanc", None)
        if orthanc is not None:
            orthanc.stop()

    def setUp(self):
        self.orthanc.posts.clear()
        self.app = Path(tempfile.mkdtemp(prefix="dicomize-app-"))
        self.other_cwd = Path(tempfile.mkdtemp(prefix="dicomize-cwd-"))
        for name in ("createDicom.php", "uploadDicom.html"):
            shutil.copy(APP_SRC / name, self.app / name)
        self.servers = []

    def tearDown(self):
        for proc in self.servers:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(self.app, ignore_errors=True)
        shutil.rmtree(self.other_cwd, ignore_errors=True)

    def _cgi_upload(self, image_bytes, filename, content_type, patient_name, patient_id):
        boundary, body = multipart_body(
            image_bytes, filename, content_type, patient_name, patient_id
        )
        env = {
            "PATH": os.environ.get("PATH", ""),
            "SCRIPT_FILENAME": str(self.app / "createDicom.php"),
            "SCRIPT_NAME": "/createDicom.php",
            "REQUEST_METHOD": "POST",
            "CONTENT_TYPE": f"multipart/form-data; boundary={boundary}",
            "CONTENT_LENGTH": str(len(body)),
            "GATEWAY_INTERFACE": "CGI/1.1",
            "SERVER_PROTOCOL": "HTTP/1.1",
            "REDIRECT_STATUS": "200",
            "QUERY_STRING": "",
        }
        completed = subprocess.run(
            [self.php_cgi, "-d", "cgi.force_redirect=0", "-d", "display_errors=1"],
            input=body,
            capture_output=True,
            cwd=self.other_cwd,
            env=env,
            check=False,
        )
        return completed.returncode, completed.stdout, completed.stderr

    def _start_static_server(self, cwd):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        proc = subprocess.Popen(
            [self.php, "-S", f"127.0.0.1:{port}", "-t", str(self.app)],
            cwd=cwd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.servers.append(proc)
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", port), 0.2):
                    return port
            except OSError:
                if proc.poll() is not None:
                    self.fail("PHP built-in server exited before accepting connections")
                time.sleep(0.05)
        self.fail("PHP built-in server did not start")

    def _http_get(self, port, path):
        url = f"http://127.0.0.1:{port}{path}"
        request = urllib.request.Request(url, method="GET")
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, response.read(), response.headers.get(
                    "Content-Type", ""
                )
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read(), exc.headers.get("Content-Type", "")

    def _http_post_upload(self, port, image_bytes, filename, content_type, patient_name, patient_id):
        boundary, body = multipart_body(
            image_bytes, filename, content_type, patient_name, patient_id
        )
        request = urllib.request.Request(
            f"http://127.0.0.1:{port}/createDicom.php",
            data=body,
            headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read()

    def _assert_upload_reached_orthanc(self, rgb):
        create_posts = [
            body
            for path, body in self.orthanc.posts
            if path == "/tools/create-dicom"
        ]
        self.assertTrue(
            create_posts,
            "createDicom.php did not POST the upload to /tools/create-dicom",
        )
        seen = pixeldata_rgb(self.php, create_posts[-1])
        self.assertEqual(
            seen,
            rgb,
            "Orthanc did not receive the re-encoded uploaded document",
        )

    def _assert_document_not_in_web_directory(self, rgb):
        scanned = self.app / "scanned.png"
        self.assertFalse(
            scanned.exists(),
            "scanned.png was left in the application directory",
        )
        unexpected = sorted(
            path.name
            for path in self.app.iterdir()
            if path.name not in {"createDicom.php", "uploadDicom.html"}
        )
        self.assertEqual(
            unexpected,
            [],
            f"upload left extra files in the web directory: {unexpected}",
        )
        port = self._start_static_server(self.other_cwd)
        status, body, content_type = self._http_get(port, "/scanned.png")
        served = png_rgb_at(self.php, body, 0, 0)
        self.assertFalse(
            status == 200 and served == rgb,
            "unauthenticated GET /scanned.png returned the uploaded document "
            f"(status {status}, content-type {content_type!r})",
        )
        self.assertNotEqual(
            status,
            200,
            "GET /scanned.png is still served from the web directory",
        )

    def test_cgi_png_upload_is_not_left_at_predictable_name(self):
        rgb = (12, 34, 56)
        image = solid_png(4, 4, rgb)
        code, stdout, stderr = self._cgi_upload(
            image, "consent.png", "image/png", "Ada Patient", "PID-100"
        )
        self.assertIn(
            b"Done",
            stdout,
            "upload handler did not finish\n"
            f"exit={code}\nstdout={stdout!r}\nstderr={stderr!r}",
        )
        self._assert_upload_reached_orthanc(rgb)
        self._assert_document_not_in_web_directory(rgb)

    def test_cgi_jpeg_upload_is_not_left_as_scanned_png(self):
        rgb = (0, 0, 0)
        image = solid_jpeg(self.php, rgb)
        self.assertTrue(image.startswith(b"\xff\xd8"), "fixture is not a JPEG")
        code, stdout, stderr = self._cgi_upload(
            image, "consent.jpg", "image/jpeg", "Grace Patient", "PID-200"
        )
        self.assertIn(
            b"Done",
            stdout,
            "JPEG upload handler did not finish\n"
            f"exit={code}\nstdout={stdout!r}\nstderr={stderr!r}",
        )
        self._assert_upload_reached_orthanc(rgb)
        self._assert_document_not_in_web_directory(rgb)

    def test_later_upload_does_not_leave_the_latest_document(self):
        first = (1, 2, 3)
        second = (9, 8, 7)
        code, stdout, stderr = self._cgi_upload(
            solid_png(4, 4, first),
            "first.png",
            "image/png",
            "First Patient",
            "PID-1",
        )
        self.assertIn(b"Done", stdout, stderr)
        code, stdout, stderr = self._cgi_upload(
            solid_png(4, 4, second),
            "second.png",
            "image/png",
            "Second Patient",
            "PID-2",
        )
        self.assertIn(
            b"Done",
            stdout,
            f"second upload did not finish\nexit={code}\nstdout={stdout!r}\nstderr={stderr!r}",
        )
        self._assert_upload_reached_orthanc(second)
        self._assert_document_not_in_web_directory(second)
        scanned = self.app / "scanned.png"
        if scanned.exists():
            self.assertNotEqual(png_rgb_at(self.php, scanned.read_bytes(), 0, 0), first)
            self.assertNotEqual(png_rgb_at(self.php, scanned.read_bytes(), 0, 0), second)

    def test_builtin_server_with_app_cwd_does_not_serve_scanned_png(self):
        rgb = (40, 50, 60)
        image = solid_png(6, 6, rgb)
        port = self._start_static_server(self.app)
        status, body = self._http_post_upload(
            port, image, "form.png", "image/png", "Lin Patient", "PID-300"
        )
        self.assertEqual(status, 200, body)
        self.assertIn(b"Done", body)
        self._assert_upload_reached_orthanc(rgb)
        get_status, get_body, content_type = self._http_get(port, "/scanned.png")
        served = png_rgb_at(self.php, get_body, 0, 0)
        self.assertFalse(
            get_status == 200 and served == rgb,
            "with the server working directory set to the application folder, "
            "GET /scanned.png returned the last upload "
            f"(status {get_status}, content-type {content_type!r})",
        )
        self.assertFalse(
            (self.app / "scanned.png").exists(),
            "scanned.png remained in the application directory",
        )


if __name__ == "__main__":
    unittest.main()
