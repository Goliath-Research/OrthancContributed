"""DICOM text in the simple viewer must be HTML-encoded.

These tests fail while patient name, patient id, and study description are
written into viewer.php without encoding. They pass once those values are
encoded before they reach the page.

Run from the repository root:

    python3 tests/test_dicom_text_html_encoding.py
"""

import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import urllib.request
import zlib
from pathlib import Path

from orthanc_083_store import reset_store, serve, styled_json

ROOT = Path(__file__).resolve().parents[1]
PHP_APP = ROOT / "Applications" / "Php"
PREPEND = Path(__file__).resolve().parent / "php7_date_constant.php"

# Even length, no quotes or backslashes: createDicom.php splices the values
# into JSON without escaping, and an even length needs no DICOM space pad.
PATIENT_NAME = "<script>self.__nm=1</script>"
PATIENT_ID = "<script>self.__id=1</script>"
STUDY_DESCRIPTION = "<script>self.__sd=1</script>"


def minimal_png():
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(b"\x00\x80")) + chunk(b"IEND", b"")


def html_text_encoding(value):
    return value.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class DicomTextHtmlEncodingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        reset_store()
        cls.orthanc = serve(8042)
        cls.php_dir = tempfile.mkdtemp(prefix="viewer-xss-")
        env = os.environ.copy()
        cls.php = subprocess.Popen(
            [
                "php",
                "-d",
                "auto_prepend_file=" + str(PREPEND),
                "-S",
                "127.0.0.1:8091",
                "-t",
                str(PHP_APP),
            ],
            cwd=cls.php_dir,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    @classmethod
    def tearDownClass(cls):
        cls.php.terminate()
        cls.php.wait(timeout=5)
        cls.orthanc.shutdown()
        shutil.rmtree(cls.php_dir, ignore_errors=True)
        # createDicom.php writes this next to itself while handling the upload.
        leftover = PHP_APP / "DicomizeImage" / "scanned.png"
        if leftover.exists():
            leftover.unlink()

    def setUp(self):
        reset_store()

    def store_and_open(self, tags, payloads):
        body = "{" + ",".join('"%s":"%s"' % (key, value) for key, value in tags.items()) + "}"
        request = urllib.request.Request(
            "http://127.0.0.1:8042/tools/create-dicom",
            data=body.encode("utf-8"),
            method="POST",
        )
        with urllib.request.urlopen(request) as response:
            created = response.read().decode("utf-8")
        instance_id = created.split('"')[3]
        with urllib.request.urlopen("http://127.0.0.1:8042/series") as response:
            series_ids = [
                part for part in response.read().decode("utf-8").split('"') if "-" in part
            ]
        series_id = None
        for candidate in series_ids:
            with urllib.request.urlopen(
                "http://127.0.0.1:8042/series/" + candidate + "/instances"
            ) as response:
                listing = response.read().decode("utf-8")
            if instance_id in listing:
                series_id = candidate
                break
        self.assertIsNotNone(series_id)
        with urllib.request.urlopen(
            "http://127.0.0.1:8091/SimpleViewerEmsy/viewer.php?series=" + series_id
        ) as response:
            page = response.read().decode("utf-8", errors="replace")
        for payload in payloads:
            self.assert_encoded(page, payload)

    def assert_encoded(self, page, payload):
        self.assertIn(
            "mcdcm - ",
            page,
            "viewer.php did not reach the patient-text output:\n" + page[:4000],
        )
        self.assertNotIn(
            payload,
            page,
            "DICOM text was written into the HTML without encoding:\n" + page[:4000],
        )
        self.assertIn(
            html_text_encoding(payload),
            page,
            "encoded DICOM text is missing from the HTML:\n" + page[:4000],
        )

    def test_styled_store_answer_matches_create_dicom_id_parse(self):
        """createDicom.php takes quote field 7 of the create-dicom JSON as the path."""
        instance_id = "aaaaaaaa-bbbbbbbb-cccccccc-dddddddd-eeeeeeee"
        body = styled_json(
            {
                "ID": instance_id,
                "Path": "/instances/" + instance_id,
                "Status": "Success",
            }
        )
        fields = body.split('"')
        self.assertEqual("/instances/" + instance_id, fields[7])

    def test_upload_form_patient_name_and_id_are_encoded(self):
        png = Path(self.php_dir) / "sample.png"
        png.write_bytes(minimal_png())
        upload = subprocess.run(
            [
                "curl",
                "-sS",
                "--form-string",
                "PatientName=" + PATIENT_NAME,
                "--form-string",
                "PatientID=" + PATIENT_ID,
                "-F",
                "file=@" + str(png) + ";type=image/png",
                "http://127.0.0.1:8091/DicomizeImage/createDicom.php",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertIn("Done", upload.stdout, upload.stdout + upload.stderr)

        with urllib.request.urlopen("http://127.0.0.1:8042/series") as response:
            series_body = response.read().decode("utf-8")
        series_id = series_body.split('"')[1]
        with urllib.request.urlopen(
            "http://127.0.0.1:8042/series/" + series_id + "/instances"
        ) as response:
            instance_id = response.read().decode("utf-8").split('"ID" : "')[1].split('"')[0]
        with urllib.request.urlopen(
            "http://127.0.0.1:8042/instances/" + instance_id + "/content/0010-0010"
        ) as response:
            stored_name = response.read().decode("utf-8")
        with urllib.request.urlopen(
            "http://127.0.0.1:8042/instances/" + instance_id + "/content/0010-0020"
        ) as response:
            stored_id = response.read().decode("utf-8")
        self.assertEqual(PATIENT_NAME, stored_name)
        self.assertEqual(PATIENT_ID, stored_id)

        # The upload script does not send Instance Number, so this instance's
        # IndexInSeries is null and viewer.php stops on PHP 8 before the echo.
        # A stored instance that also has Instance Number is what the viewer
        # divides and then prints. Create that instance with the same text.
        self.store_and_open(
            {
                "PatientName": PATIENT_NAME,
                "PatientID": PATIENT_ID,
                "SOPClassUID": "1.2.840.10008.5.1.4.1.1.7",
                "Modality": "SC",
                "InstanceNumber": "1",
                "PixelData": "data:image/png;base64,AA==",
            },
            [PATIENT_NAME, PATIENT_ID],
        )

    def test_stored_study_description_is_encoded(self):
        # uploadDicom.html does not send StudyDescription. The same
        # create-dicom route the upload script calls accepts it, and
        # viewer.php echoes tag 0008-1030. Store that tag the way
        # ParseReplacements stores every other string tag.
        self.store_and_open(
            {
                "PatientName": "Plain",
                "SOPClassUID": "1.2.840.10008.5.1.4.1.1.7",
                "Modality": "SC",
                "StudyDescription": STUDY_DESCRIPTION,
                "InstanceNumber": "1",
                "PixelData": "data:image/png;base64,AA==",
            },
            [STUDY_DESCRIPTION],
        )


if __name__ == "__main__":
    unittest.main()
