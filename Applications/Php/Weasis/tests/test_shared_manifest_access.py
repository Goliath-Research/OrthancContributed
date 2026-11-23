#!/usr/bin/env python3
"""Security properties for the Weasis launcher in this directory.

The tests drive the real loadWeasisX.php through PHP's built-in server, with
Orthanc's patient/study/series/instance JSON standing in for the REST bodies
documented in the Orthanc Book. They follow the launcher the way Weasis 1.1.2
does: the JNLP argument that starts with "$" becomes a dicom:get command, "-w"
is the manifest URL, that URL is fetched with a plain GET, and each instance
URL is the manifest's wadoURL joined with DirectDownloadFile.

These tests fail while the bug is present. They pass once a launch no longer
publishes or replaces one shared web-visible manifest.

Contract a fix can satisfy:

- Authorization: Bearer weasis-test-user is an authenticated staff session.
  weasis-test-user is a placeholder for this test, not a deployed credential.
- Any request without that header is refused with 401 or 403. Refusing must
  not create, modify, or delete a file under the web root.
- An accepted launch returns a document whose dicom:get -w URL answers a plain
  GET (Weasis does not send the staff header when it downloads that URL).
  The body is that launch's patient only: name, birth date, study description,
  and DirectDownloadFile.
- A later request must not change the bytes of a manifest already issued.
  That includes an anonymous request, a request with an unknown patient id,
  a request with no patient id, and a later accepted launch for someone else.
- http://10.1.20.127/pacs/weasisX.xml stays free of patient identifiers. That
  is the address hard-coded in weasisX.jnlp.

Run from the repository root:

    python3 Applications/Php/Weasis/tests/test_shared_manifest_access.py

Requires the php CLI and passwordless sudo (the launcher's hard-coded web
host is port 80, and its hard-coded Orthanc host is bound on the loopback
interface).
"""

from __future__ import annotations

import ctypes
import hashlib
import os
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

WEASIS_DIR = Path(__file__).resolve().parents[1]
SOURCE_PHP = WEASIS_DIR / "loadWeasisX.php"
SOURCE_JNLP = WEASIS_DIR / "weasisX.jnlp"

WEB_HOST = "10.1.20.127"
ORTHANC_HOST = "10.2.150.33"
ORTHANC_PORT = 8042
# Placeholder staff session. Not a deployed credential.
STAFF_BEARER = "weasis-test-user"
FIXED_MANIFEST = f"http://{WEB_HOST}/pacs/weasisX.xml"

ALICE = {
    "uuid": "patient-a",
    "name": "DOE^ALICE",
    "pid": "MRN-A-100",
    "birth": "19750304",
    "study": "study-a",
    "study_uid": "1.2.840.study.a",
    "study_desc": "CT Head Alice",
    "study_date": "20240101",
    "study_time": "101010",
    "series": "series-a",
    "series_uid": "1.2.840.series.a",
    "series_desc": "Axial",
    "series_number": "1",
    "modality": "CT",
    "instance": "instance-a",
    "sop": "1.2.840.sop.a",
    "inum": "1",
}
BOB = {
    "uuid": "patient-b",
    "name": "ROE^BOB",
    "pid": "MRN-B-200",
    "birth": "19881212",
    "study": "study-b",
    "study_uid": "1.2.840.study.b",
    "study_desc": "MR Spine Bob",
    "study_date": "20240202",
    "study_time": "111111",
    "series": "series-b",
    "series_uid": "1.2.840.series.b",
    "series_desc": "Sagittal",
    "series_number": "2",
    "modality": "MR",
    "instance": "instance-b",
    "sop": "1.2.840.sop.b",
    "inum": "2",
}
PATIENTS = {ALICE["uuid"]: ALICE, BOB["uuid"]: BOB}

# inotify masks: CREATE, DELETE, MODIFY, CLOSE_WRITE, MOVED_FROM, MOVED_TO
_INOTIFY_MASK = 0x100 | 0x200 | 0x2 | 0x8 | 0x40 | 0x80


# JSON whitespace matches the Orthanc Book samples the PHP parser reads.
_ORTHANC_SERVER = r"""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PATIENTS = json.loads(r'''%PATIENTS%''')
INDEX = {}
for record in PATIENTS:
    INDEX[("patients", record["uuid"])] = ("patient", record)
    INDEX[("studies", record["study"])] = ("study", record)
    INDEX[("series", record["series"])] = ("series", record)
    INDEX[("instances", record["instance"])] = ("instance", record)

def body_for(kind, record):
    if kind == "patient":
        return (
            '{\n'
            '  "ID" : "%s",\n'
            '  "MainDicomTags" : {\n'
            '     "OtherPatientIDs" : "(null)",\n'
            '     "PatientBirthDate" : "%s",\n'
            '     "PatientID" : "%s",\n'
            '     "PatientName" : "%s",\n'
            '     "PatientSex" : "O"\n'
            '  },\n'
            '  "Studies" : [ "%s" ],\n'
            '  "Type" : "Patient"\n'
            '}\n'
        ) % (record["uuid"], record["birth"], record["pid"], record["name"], record["study"])
    if kind == "study":
        return (
            '{\n'
            '  "ID" : "%s",\n'
            '  "MainDicomTags" : {\n'
            '     "AccessionNumber" : "(null)",\n'
            '     "StudyDate" : "%s",\n'
            '     "StudyDescription" : "%s",\n'
            '     "StudyID" : "23848",\n'
            '     "StudyInstanceUID" : "%s",\n'
            '     "StudyTime" : "%s"\n'
            '  },\n'
            '  "ParentPatient" : "%s",\n'
            '  "Series" : [ "%s" ],\n'
            '  "Type" : "Study"\n'
            '}\n'
        ) % (
            record["study"], record["study_date"], record["study_desc"],
            record["study_uid"], record["study_time"], record["uuid"], record["series"],
        )
    if kind == "series":
        return (
            '{\n'
            '  "ExpectedNumberOfInstances" : 1,\n'
            '  "ID" : "%s",\n'
            '  "Instances" : [ "%s" ],\n'
            '  "MainDicomTags" : {\n'
            '     "Manufacturer" : "Philips Medical Systems",\n'
            '     "Modality" : "%s",\n'
            '     "SeriesDescription" : "%s",\n'
            '     "SeriesInstanceUID" : "%s",\n'
            '     "SeriesNumber" : "%s"\n'
            '  },\n'
            '  "ParentStudy" : "%s",\n'
            '  "Type" : "Series"\n'
            '}\n'
        ) % (
            record["series"], record["instance"], record["modality"],
            record["series_desc"], record["series_uid"], record["series_number"],
            record["study"],
        )
    return (
        '{\n'
        '  "FileSize" : 70356,\n'
        '  "ID" : "%s",\n'
        '  "IndexInSeries" : 1,\n'
        '  "MainDicomTags" : {\n'
        '     "InstanceNumber" : "%s",\n'
        '     "SOPInstanceUID" : "%s"\n'
        '  },\n'
        '  "ParentSeries" : "%s",\n'
        '  "Type" : "Instance"\n'
        '}\n'
    ) % (record["instance"], record["inum"], record["sop"], record["series"])

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        with open(r"%LOG%", "a") as handle:
            handle.write("%s %s\n" % (self.command, self.path))

    def do_GET(self):
        parts = [part for part in self.path.split("?")[0].split("/") if part]
        code = 404
        body = '{"error":"not found"}\n'
        if len(parts) == 2 and (parts[0], parts[1]) in INDEX:
            kind, record = INDEX[(parts[0], parts[1])]
            code = 200
            body = body_for(kind, record)
        elif len(parts) == 3 and parts[0] == "instances" and parts[2] == "file":
            kind, record = INDEX.get(("instances", parts[1]), (None, None))
            if record is not None:
                code = 200
                body = "DICOM %s\n" % record["instance"]
        data = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

ThreadingHTTPServer(("%HOST%", %PORT%), Handler).serve_forever()
"""


class DirWatch:
    """Records create, delete, modify, and rename events under one directory."""

    def __init__(self, path: Path):
        self.libc = ctypes.CDLL("libc.so.6", use_errno=True)
        self.fd = self.libc.inotify_init1(0)
        if self.fd < 0:
            raise OSError(ctypes.get_errno(), "inotify_init1")
        watch = self.libc.inotify_add_watch(self.fd, os.fsencode(path), _INOTIFY_MASK)
        if watch < 0:
            os.close(self.fd)
            raise OSError(ctypes.get_errno(), "inotify_add_watch")
        os.set_blocking(self.fd, False)
        self.events: list[str] = []

    def collect(self) -> list[str]:
        import select

        while True:
            ready, _, _ = select.select([self.fd], [], [], 0)
            if not ready:
                break
            raw = os.read(self.fd, 4096)
            offset = 0
            while offset + 16 <= len(raw):
                header = raw[offset : offset + 16]
                mask = int.from_bytes(header[4:8], "little")
                name_len = int.from_bytes(header[12:16], "little")
                name = raw[offset + 16 : offset + 16 + name_len].split(b"\x00", 1)[0]
                offset += 16 + name_len
                bits = []
                for label, bit in (
                    ("create", 0x100),
                    ("delete", 0x200),
                    ("modify", 0x2),
                    ("close_write", 0x8),
                    ("moved_from", 0x40),
                    ("moved_to", 0x80),
                ):
                    if mask & bit:
                        bits.append(label)
                if bits:
                    self.events.append(
                        "%s %s" % (",".join(bits), name.decode() or ".")
                    )
        return list(self.events)

    def close(self) -> None:
        os.close(self.fd)


def _snapshot(root: Path) -> dict[str, str]:
    found = {}
    for path in sorted(root.rglob("*")):
        if path.is_file() and not path.is_symlink():
            found[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return found


def _http(url: str, headers: dict | None = None) -> tuple[int, bytes]:
    request = urllib.request.Request(url, headers=headers or {})
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def _staff_headers() -> dict[str, str]:
    return {"Authorization": "Bearer " + STAFF_BEARER}


def split_jnlp_commands(argument_texts: list[str]) -> list[str]:
    """WeasisLauncher.splitCommand from Weasis 1.1.2.

    A JNLP argument that starts with "$" is a command. The "$" is removed and
    following arguments are appended until the next "$".
    """
    commands = []
    index = 0
    while index < len(argument_texts):
        arg = argument_texts[index]
        if arg.startswith("$") and len(arg) > 1:
            command = arg[1:]
            while index + 1 < len(argument_texts) and not argument_texts[index + 1].startswith("$"):
                index += 1
                command += " "
                piece = argument_texts[index]
                if " " in piece:
                    command += '"' + piece + '"'
                else:
                    command += piece
            commands.append(command.strip())
        index += 1
    return commands


def jnlp_argument_texts(jnlp_bytes: bytes) -> list[str]:
    root = ET.fromstring(jnlp_bytes)
    texts = []
    for element in root.iter():
        if _local(element.tag) == "argument" and element.text:
            texts.append(element.text.strip())
    return texts


def wado_manifest_urls(jnlp_bytes: bytes) -> list[str]:
    urls = []
    for command in split_jnlp_commands(jnlp_argument_texts(jnlp_bytes)):
        parts = command.split()
        if not parts or parts[0] != "dicom:get":
            continue
        for index, part in enumerate(parts):
            if part == "-w" and index + 1 < len(parts):
                urls.append(parts[index + 1])
    return urls


def _local(tag: str) -> str:
    if tag.startswith("{"):
        return tag.split("}", 1)[1]
    return tag


def parse_manifest(xml_bytes: bytes) -> dict:
    """Fields DownloadManager.readPatient/readStudy/readSeries take from wado_query."""
    root = ET.fromstring(xml_bytes)
    if _local(root.tag) != "wado_query":
        raise AssertionError("manifest root is %s, expected wado_query" % _local(root.tag))
    patient = next(element for element in root.iter() if _local(element.tag) == "Patient")
    study = next(element for element in root.iter() if _local(element.tag) == "Study")
    instance = next(element for element in root.iter() if _local(element.tag) == "Instance")
    wado_url = root.attrib["wadoURL"]
    direct = instance.attrib.get("DirectDownloadFile", "")
    # LoadSeries builds the instance URL as wadoURL + DirectDownloadFile.
    instance_url = wado_url + direct
    return {
        "name": patient.attrib.get("PatientName", ""),
        "birth": patient.attrib.get("PatientBirthDate", ""),
        "study": study.attrib.get("StudyDescription", ""),
        "direct": direct,
        "instance_url": instance_url,
    }


def launch_urls_from_response(body: bytes) -> list[str]:
    text = body.decode("utf-8", "replace")
    urls = []
    marker = 'window.location = "'
    start = 0
    while True:
        at = text.find(marker, start)
        if at < 0:
            break
        end = text.find('"', at + len(marker))
        urls.append(text[at + len(marker) : end])
        start = end + 1
    if b"<jnlp" in body or b"dicom:get" in body:
        urls.append("inline-jnlp")
    return urls


class SharedManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SOURCE_PHP.is_file() or not SOURCE_JNLP.is_file():
            raise unittest.SkipTest("launcher sources are not in the tree")
        subprocess.run(
            ["sudo", "-n", "ip", "addr", "add", ORTHANC_HOST + "/32", "dev", "lo"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.run(
            ["sudo", "-n", "ip", "addr", "add", WEB_HOST + "/32", "dev", "lo"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        cls.work = Path(tempfile.mkdtemp(prefix="weasis-manifest-"))
        cls.docroot = cls.work / "web"
        cls.pacs = cls.docroot / "pacs"
        cls.pacs.mkdir(parents=True)
        shutil.copy(SOURCE_PHP, cls.pacs / "loadWeasisX.php")
        shutil.copy(SOURCE_JNLP, cls.pacs / "weasisX.jnlp")
        cls.orthanc_log = cls.work / "orthanc.log"
        server = _ORTHANC_SERVER.replace("%PATIENTS%", __import__("json").dumps(list(PATIENTS.values())))
        server = server.replace("%LOG%", str(cls.orthanc_log))
        server = server.replace("%HOST%", ORTHANC_HOST)
        server = server.replace("%PORT%", str(ORTHANC_PORT))
        server_path = cls.work / "orthanc_mock.py"
        server_path.write_text(server)
        cls.orthanc = subprocess.Popen(
            ["python3", str(server_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        cls.php_log = cls.work / "php.log"
        php_log = open(cls.php_log, "w")
        # cwd is the pacs web directory, which is where the contributed script
        # is deployed and where its relative fopen("weasisX.xml") is served.
        cls.php = subprocess.Popen(
            ["sudo", "-n", "php", "-S", WEB_HOST + ":80", "-t", str(cls.docroot)],
            cwd=cls.pacs,
            stdout=php_log,
            stderr=subprocess.STDOUT,
        )
        cls._php_log_handle = php_log
        deadline = time.time() + 5
        last_error = None
        while time.time() < deadline:
            try:
                with socket.create_connection((ORTHANC_HOST, ORTHANC_PORT), 0.2):
                    pass
                with socket.create_connection((WEB_HOST, 80), 0.2):
                    pass
                last_error = None
                break
            except OSError as error:
                last_error = error
                time.sleep(0.05)
        if last_error is not None:
            raise RuntimeError("launcher test servers did not start: %s" % last_error)

    @classmethod
    def tearDownClass(cls):
        for process in (getattr(cls, "php", None), getattr(cls, "orthanc", None)):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
        handle = getattr(cls, "_php_log_handle", None)
        if handle is not None:
            handle.close()
        work = getattr(cls, "work", None)
        if work is not None:
            subprocess.run(["sudo", "-n", "rm", "-rf", str(work)], check=False)

    def setUp(self):
        for path in self.pacs.iterdir():
            if path.name in ("loadWeasisX.php", "weasisX.jnlp"):
                continue
            if path.is_file() or path.is_symlink():
                subprocess.run(["sudo", "-n", "rm", "-f", str(path)], check=True)

    def test_anonymous_launch_does_not_touch_the_web_root(self):
        before = _snapshot(self.docroot)
        watch = DirWatch(self.pacs)
        try:
            status, body = _http(
                "http://%s/pacs/loadWeasisX.php?PatientUUID=%s" % (WEB_HOST, ALICE["uuid"])
            )
            # Let a write that the request already finished flush to the watcher.
            time.sleep(0.05)
            events = watch.collect()
        finally:
            watch.close()
        after = _snapshot(self.docroot)
        with self.subTest("status"):
            self.assertIn(status, (401, 403), "anonymous launch returned %s" % status)
        with self.subTest("files"):
            self.assertEqual(before, after, "anonymous launch changed files in the web root")
        with self.subTest("events"):
            self.assertEqual(events, [], "anonymous launch changed the web root: %s" % events)
        text = body.decode("utf-8", "replace")
        for secret in (ALICE["name"], ALICE["birth"], ALICE["study_desc"]):
            with self.subTest("response body", secret=secret):
                self.assertNotIn(secret, text)

    def test_issued_manifest_stays_bound_to_that_patient(self):
        status, body = _http(
            "http://%s/pacs/loadWeasisX.php?PatientUUID=%s" % (WEB_HOST, ALICE["uuid"]),
            _staff_headers(),
        )
        self.assertEqual(status, 200, "staff launch failed: %s %s" % (status, body[:200]))
        manifest_url, manifest = self._manifest_for_launch(body)
        self.assertEqual(manifest["name"], ALICE["name"])
        self.assertEqual(manifest["birth"], ALICE["birth"])
        self.assertEqual(manifest["study"], ALICE["study_desc"])
        self.assertEqual(manifest["direct"], ALICE["instance"] + "/file")
        issued = _http(manifest_url)[1]

        fixed_status, fixed_body = _http(FIXED_MANIFEST)
        fixed_text = fixed_body.decode("utf-8", "replace")
        with self.subTest("fixed public manifest"):
            self.assertNotIn(ALICE["name"], fixed_text)
            self.assertNotIn(ALICE["birth"], fixed_text)
            self.assertNotIn(ALICE["study_desc"], fixed_text)
            self.assertNotIn(ALICE["instance"] + "/file", fixed_text)

        # The interfering calls happen after the manifest URL was issued and
        # before it is read again. A fix that looks up one shared path at
        # download time, or that writes the file before refusing, fails here.
        _http("http://%s/pacs/loadWeasisX.php?PatientUUID=%s" % (WEB_HOST, BOB["uuid"]))
        _http("http://%s/pacs/loadWeasisX.php?PatientUUID=missing-patient" % WEB_HOST)
        _http("http://%s/pacs/loadWeasisX.php" % WEB_HOST)
        _http(
            "http://%s/pacs/loadWeasisX.php?PatientUUID=%s" % (WEB_HOST, BOB["uuid"]),
            _staff_headers(),
        )

        again_status, again = _http(manifest_url)
        with self.subTest("manifest unchanged after later launches"):
            self.assertEqual(again_status, 200)
            self.assertEqual(again, issued)
            parsed = parse_manifest(again)
            self.assertEqual(parsed["name"], ALICE["name"])
            self.assertEqual(parsed["birth"], ALICE["birth"])
            self.assertEqual(parsed["study"], ALICE["study_desc"])
            self.assertEqual(parsed["direct"], ALICE["instance"] + "/file")
            self.assertNotIn(BOB["name"], again.decode("utf-8", "replace"))

        fixed_text = _http(FIXED_MANIFEST)[1].decode("utf-8", "replace")
        with self.subTest("fixed manifest after replacement attempts"):
            for secret in (
                ALICE["name"],
                ALICE["birth"],
                ALICE["study_desc"],
                BOB["name"],
                BOB["birth"],
                BOB["study_desc"],
            ):
                self.assertNotIn(secret, fixed_text)

        # The instance list the manifest names is the one Weasis would open.
        instance_status, instance_body = _http(manifest["instance_url"])
        self.assertEqual(instance_status, 200)
        self.assertIn(ALICE["instance"].encode(), instance_body)
        self.assertNotIn(BOB["instance"].encode(), instance_body)

    def _manifest_for_launch(self, body: bytes) -> tuple[str, dict]:
        urls = launch_urls_from_response(body)
        jnlp_blobs = []
        if b"<jnlp" in body or b"dicom:get" in body:
            jnlp_blobs.append(body)
        for url in urls:
            if url == "inline-jnlp":
                continue
            status, fetched = _http(url)
            self.assertEqual(status, 200, "launcher document %s returned %s" % (url, status))
            jnlp_blobs.append(fetched)
        manifest_urls = []
        for blob in jnlp_blobs:
            manifest_urls.extend(wado_manifest_urls(blob))
        self.assertTrue(manifest_urls, "launch response did not name a dicom:get -w manifest")
        manifest_url = manifest_urls[0]
        status, manifest_body = _http(manifest_url)
        self.assertEqual(status, 200, "manifest %s returned %s" % (manifest_url, status))
        return manifest_url, parse_manifest(manifest_body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
