#!/usr/bin/env python3
"""Access-control tests for the shipped VPI Reveal Orthanc configuration.

The operator instructions tell an administrator to copy
``Plugins/orthancVPIRevealPlugin/Configuration.json`` onto the Orthanc
server and restart it. These tests start Orthanc with the security
settings from that file and connect from a non-loopback address with
no credentials.

They expect the archive to refuse that client:

* listing patients and studies
* uploading an instance
* downloading an instance file
* deleting a study and a patient
* storing an image over DICOM when the called AE title is not the
  configured ``DicomAet``

Run from the repository root:

    python3 -m unittest Plugins.orthancVPIRevealPlugin.tests.test_unauthenticated_remote_access

The Orthanc executable on ``PATH`` is used, or ``ORTHANC_BIN`` when set.
``storescu`` and ``dump2dcm`` (DCMTK) must be on ``PATH``.
"""

from __future__ import annotations

import http.client
import json
import os
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import uuid
from pathlib import Path


CONFIGURATION = (
    Path(__file__).resolve().parents[1] / "Configuration.json"
)

# Keys that decide whether a remote client can read or write the archive.
# The runtime file keeps these values exactly as shipped. Only paths and
# port numbers are rewritten so the server can start on this host: the
# plugin entry is a Windows DLL, and the HTTP/DICOM ports must be free.
SECURITY_KEYS = (
    "HttpServerEnabled",
    "RemoteAccessAllowed",
    "AuthenticationEnabled",
    "RegisteredUsers",
    "SslEnabled",
    "DicomServerEnabled",
    "DicomCheckCalledAet",
    "DicomAet",
    "StrictAetComparison",
    "DicomAlwaysAllowStore",
    "DicomCheckModalityHost",
)


def strip_json_comments(text: str) -> str:
    """Remove ``//`` and ``/* */`` comments the way a JSON-with-comments
    reader does, without touching comment markers that sit inside strings.
    """
    out = []
    i = 0
    length = len(text)
    in_string = False
    escaped = False
    while i < length:
        char = text[i]
        if in_string:
            out.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            i += 1
            continue
        if char == '"':
            in_string = True
            out.append(char)
            i += 1
            continue
        if char == "/" and i + 1 < length and text[i + 1] == "/":
            i += 2
            while i < length and text[i] != "\n":
                i += 1
            continue
        if char == "/" and i + 1 < length and text[i + 1] == "*":
            i += 2
            while i + 1 < length and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i = min(i + 2, length)
            continue
        out.append(char)
        i += 1
    return "".join(out)


def load_configuration(path: Path) -> dict:
    return json.loads(strip_json_comments(path.read_text(encoding="utf-8")))


def non_loopback_ipv4() -> str:
    """Return a local IPv4 address Orthanc will not treat as localhost.

    Orthanc 1.1.0 and 1.12.2 both treat only ``127.0.0.1`` as local.
    """
    for iface in sorted(os.listdir("/sys/class/net")):
        if iface == "lo":
            continue
        state_path = Path("/sys/class/net") / iface / "operstate"
        if state_path.is_file() and state_path.read_text().strip() == "down":
            continue
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            import fcntl
            import struct

            packed = fcntl.ioctl(
                sock.fileno(),
                0x8915,  # SIOCGIFADDR
                struct.pack("256s", iface.encode("utf-8"))[:256],
            )
        except OSError:
            continue
        finally:
            sock.close()
        address = socket.inet_ntoa(packed[20:24])
        if not address.startswith("127."):
            return address
    raise unittest.SkipTest("no non-loopback IPv4 address is available")


def free_tcp_port() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("0.0.0.0", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def make_dicom(directory: Path, patient_name: str) -> tuple[Path, str]:
    """Build a small secondary-capture instance with DCMTK ``dump2dcm``."""
    sop_uid = "1.2.826.0.1.3680043.10.1541." + uuid.uuid4().hex
    study_uid = "1.2.826.0.1.3680043.10.1541." + uuid.uuid4().hex
    series_uid = "1.2.826.0.1.3680043.10.1541." + uuid.uuid4().hex
    patient_id = "PID" + uuid.uuid4().hex[:12]
    dump = directory / (patient_id + ".dump")
    dcm = directory / (patient_id + ".dcm")
    dump.write_text(
        "\n".join(
            [
                "(0008,0016) UI [1.2.840.10008.5.1.4.1.1.7]",
                "(0008,0018) UI [%s]" % sop_uid,
                "(0008,0060) CS [OT]",
                "(0010,0010) PN [%s]" % patient_name,
                "(0010,0020) LO [%s]" % patient_id,
                "(0020,000d) UI [%s]" % study_uid,
                "(0020,000e) UI [%s]" % series_uid,
                "(0028,0002) US 1",
                "(0028,0004) CS [MONOCHROME2]",
                "(0028,0010) US 1",
                "(0028,0011) US 1",
                "(0028,0100) US 8",
                "(0028,0101) US 8",
                "(0028,0102) US 7",
                "(0028,0103) US 0",
                "(7fe0,0010) OB 00",
                "",
            ]
        ),
        encoding="ascii",
    )
    subprocess.run(
        ["dump2dcm", "--write-xfer-little", str(dump), str(dcm)],
        check=True,
        capture_output=True,
    )
    return dcm, sop_uid


class HttpResult:
    def __init__(self, status: int | None, body: bytes, error: str | None = None):
        self.status = status
        self.body = body
        self.error = error

    def denied(self) -> bool:
        """True when the server did not carry out the request.

        Connection failure and HTTP 401/403 are refusals. A 2xx response
        means the archive performed the operation.
        """
        if self.error is not None or self.status is None:
            return True
        return self.status in (401, 403)


def request(host: str, port: int, method: str, path: str, body: bytes | None = None,
            headers: dict | None = None) -> HttpResult:
    connection = http.client.HTTPConnection(host, port, timeout=10)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        payload = response.read()
        return HttpResult(response.status, payload)
    except (OSError, http.client.HTTPException, TimeoutError) as exc:
        return HttpResult(None, b"", str(exc))
    finally:
        connection.close()


class UnauthenticatedRemoteAccessTest(unittest.TestCase):
    """The shipped configuration must not let a remote client in."""

    @classmethod
    def setUpClass(cls):
        if shutil.which("dump2dcm") is None or shutil.which("storescu") is None:
            raise unittest.SkipTest("DCMTK dump2dcm and storescu are required")
        orthanc = os.environ.get("ORTHANC_BIN", "Orthanc")
        if shutil.which(orthanc) is None and not Path(orthanc).is_file():
            raise unittest.SkipTest("Orthanc executable not found")

        cls.shipped = load_configuration(CONFIGURATION)
        cls.remote_host = non_loopback_ipv4()
        cls.work = Path(tempfile.mkdtemp(prefix="vpi-access-"))
        cls.storage = cls.work / "storage"
        cls.index = cls.work / "index"
        cls.storage.mkdir()
        cls.index.mkdir()
        cls.http_port = free_tcp_port()
        cls.dicom_port = free_tcp_port()

        runtime = dict(cls.shipped)
        runtime["StorageDirectory"] = str(cls.storage)
        runtime["IndexDirectory"] = str(cls.index)
        # The shipped plugin path is a Windows DLL. It does not implement
        # HTTP or DICOM access control; Orthanc core does. Drop it so the
        # server can start on this host without changing security keys.
        runtime["Plugins"] = []
        runtime["HttpPort"] = cls.http_port
        runtime["DicomPort"] = cls.dicom_port
        for key in SECURITY_KEYS:
            if cls.shipped.get(key) != runtime.get(key):
                raise AssertionError("runtime config changed security key %s" % key)

        cls.config_path = cls.work / "Configuration.json"
        cls.config_path.write_text(json.dumps(runtime), encoding="utf-8")
        cls.log_path = cls.work / "orthanc.log"
        cls.log_file = cls.log_path.open("w", encoding="utf-8")
        cls.server = subprocess.Popen(
            [orthanc, str(cls.config_path)],
            stdout=cls.log_file,
            stderr=subprocess.STDOUT,
        )
        cls._wait_until_started()

    @classmethod
    def tearDownClass(cls):
        server = getattr(cls, "server", None)
        if server is not None and server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
        log_file = getattr(cls, "log_file", None)
        if log_file is not None:
            log_file.close()
        work = getattr(cls, "work", None)
        if work is not None:
            shutil.rmtree(work, ignore_errors=True)

    @classmethod
    def _wait_until_started(cls):
        deadline = time.time() + 20
        while time.time() < deadline:
            if cls.server.poll() is not None:
                raise AssertionError(
                    "Orthanc exited before listening:\n" + cls.log_path.read_text(errors="replace")
                )
            text = cls.log_path.read_text(errors="replace")
            if "HTTP server listening" in text and (
                "DICOM server listening" in text or "DICOM server is disabled" in text
            ):
                return
            time.sleep(0.1)
        raise AssertionError(
            "Orthanc did not listen in time:\n" + cls.log_path.read_text(errors="replace")
        )

    @classmethod
    def _local(cls, method: str, path: str, body: bytes | None = None,
               headers: dict | None = None) -> HttpResult:
        return request("127.0.0.1", cls.http_port, method, path, body, headers)

    def _remote(self, method: str, path: str, body: bytes | None = None,
                headers: dict | None = None) -> HttpResult:
        return request(self.remote_host, self.http_port, method, path, body, headers)

    def _upload_local(self, patient_name: str) -> dict | None:
        """Store one instance from localhost when the server allows it.

        Returns the upload JSON plus the SOP Instance UID, or None when
        localhost itself requires credentials. Each caller uses its own
        instance so one operation cannot remove the object under test.
        """
        dcm, sop_uid = make_dicom(self.work, patient_name)
        uploaded = self._local(
            "POST",
            "/instances",
            dcm.read_bytes(),
            {"Content-Type": "application/dicom"},
        )
        if uploaded.status != 200:
            return None
        try:
            payload = json.loads(uploaded.body.decode("utf-8"))
        except json.JSONDecodeError:
            return None
        if "ID" not in payload or "ParentStudy" not in payload or "ParentPatient" not in payload:
            return None
        payload["SOPInstanceUID"] = sop_uid
        return payload

    def _assert_refused(self, result: HttpResult, operation: str):
        self.assertTrue(
            result.denied(),
            "%s from %s was accepted (HTTP %s). A remote client with no "
            "credentials must be refused."
            % (operation, self.remote_host, result.status),
        )

    def test_remote_client_cannot_list_patients(self):
        result = self._remote("GET", "/patients")
        self._assert_refused(result, "GET /patients")

    def test_remote_client_cannot_list_studies(self):
        result = self._remote("GET", "/studies")
        self._assert_refused(result, "GET /studies")

    def test_remote_client_cannot_upload_an_instance(self):
        before = self._instance_ids()
        upload_dcm, sop_uid = make_dicom(self.work, "UploadedPatient")
        result = self._remote(
            "POST",
            "/instances",
            upload_dcm.read_bytes(),
            {"Content-Type": "application/dicom"},
        )
        after = self._instance_ids()
        stored = sop_uid.encode("ascii") in result.body or (
            before is not None and after is not None and set(after) - set(before)
        )
        self._assert_refused(result, "POST /instances")
        self.assertFalse(
            stored,
            "POST /instances stored a DICOM instance before refusing the remote client",
        )
        if before is not None and after is not None:
            self.assertEqual(
                sorted(before),
                sorted(after),
                "the instance index grew after a remote upload that should have been refused",
            )

    def test_remote_client_cannot_download_an_instance_file(self):
        seeded = self._upload_local("DownloadPatient")
        if seeded is None:
            result = self._remote("GET", "/instances/missing-instance/file")
            self._assert_refused(result, "GET /instances/{id}/file")
            return
        result = self._remote("GET", "/instances/%s/file" % seeded["ID"])
        self._assert_refused(result, "GET /instances/{id}/file")
        self.assertNotIn(
            seeded["SOPInstanceUID"].encode("ascii"),
            result.body,
            "the DICOM instance file was downloaded by a remote client with no credentials",
        )

    def test_remote_client_cannot_delete_a_study(self):
        self._assert_delete_leaves_resource("ParentStudy", "/studies/%s", "DELETE /studies/{id}")

    def test_remote_client_cannot_delete_a_patient(self):
        self._assert_delete_leaves_resource(
            "ParentPatient", "/patients/%s", "DELETE /patients/{id}"
        )

    def test_dicom_store_rejects_a_called_ae_title_that_does_not_match(self):
        """C-STORE must not archive an image when the called AE title is wrong.

        ``DicomCheckCalledAet`` false makes Orthanc accept every called title.
        The store also has to be refused when the DICOM server is disabled.
        """
        before = self._instance_ids()
        store_dcm, _sop_uid = make_dicom(self.work, "DicomStorePatient")
        configured_aet = str(self.shipped.get("DicomAet", "ORTHANC"))
        called_aet = "WRONG-AET"
        self.assertNotEqual(called_aet.casefold(), configured_aet.casefold())
        completed = subprocess.run(
            [
                "storescu",
                "-aet",
                "CALLING",
                "-aec",
                called_aet,
                self.remote_host,
                str(self.dicom_port),
                str(store_dcm),
            ],
            capture_output=True,
            timeout=20,
        )
        after = self._instance_ids()
        grew = before is not None and after is not None and set(after) - set(before)
        self.assertFalse(
            grew or completed.returncode == 0,
            "DICOM C-STORE to %s:%s with called AE title %s was accepted "
            "(storescu exit %s). The called AE title is %s and was not checked.\n%s"
            % (
                self.remote_host,
                self.dicom_port,
                called_aet,
                completed.returncode,
                configured_aet,
                completed.stderr.decode("utf-8", errors="replace")[-1000:],
            ),
        )
        if before is not None and after is not None:
            self.assertEqual(sorted(before), sorted(after))

    def _assert_delete_leaves_resource(self, seed_field: str, path_template: str, operation: str):
        seeded = self._upload_local("Delete" + seed_field)
        if seeded is None:
            result = self._remote("DELETE", path_template % "missing-resource")
            self._assert_refused(result, operation)
            return
        resource_id = seeded[seed_field]
        before = self._local("GET", path_template % resource_id)
        self.assertEqual(
            before.status,
            200,
            "seeded resource was not readable from localhost before the delete attempt",
        )
        result = self._remote("DELETE", path_template % resource_id)
        after = self._local("GET", path_template % resource_id)
        removed = after.status != 200 or before.body != after.body
        self.assertFalse(
            (not result.denied()) or removed,
            "%s from %s was accepted (HTTP %s) and the resource %s was %s."
            % (
                operation,
                self.remote_host,
                result.status,
                resource_id,
                "removed" if removed else "left in place",
            ),
        )

    def _instance_ids(self) -> list | None:
        listed = self._local("GET", "/instances")
        if listed.status != 200:
            return None
        try:
            payload = json.loads(listed.body.decode("utf-8"))
        except json.JSONDecodeError:
            return None
        if isinstance(payload, list):
            return payload
        return None


if __name__ == "__main__":
    unittest.main()
