"""Local stand-in for the Orthanc 0.8.3 routes this PHP viewer calls.

The transformations below are the ones read from Orthanc 0.8.3 and the
libraries it ships (JsonCpp 0.6.0-rc2, DCMTK 3.6.0). This is not a DICOM
parser. It applies those steps to the tag strings and emits the HTTP bodies
the PHP then reads.
"""

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


# 1x1 PNG. image-uint16 answers image/png (OrthancRestResources.cpp GetImage).
# The viewer only needs an IHDR chunk so it can continue to the tag echoes.
MINIMAL_PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010802000000907753"
    "de0000000c4944415408d763f8ffff3f0005fe02fe4c22c8b80000000049454e44"
    "ae426082"
)

TAG_BY_NAME = {
    "PatientName": "0010-0010",
    "PatientID": "0010-0020",
    "StudyDescription": "0008-1030",
    "SeriesDescription": "0008-103e",
    "SOPClassUID": "0008-0016",
    "Modality": "0008-0060",
    "StudyDate": "0008-0020",
    "SeriesDate": "0008-0021",
    "StudyTime": "0008-0030",
    "SeriesTime": "0008-0031",
    "InstanceNumber": "0020-0013",
}


def index_in_series(tags):
    raw = tags.get("0020-0013")
    if raw is None or raw == "":
        return None
    return int(raw)


def dicom_string_bytes(value):
    """Bytes AnswerDicomField sends for a string VR.

    putString stores the caller bytes (dcbytstr.cc DcmByteString::putString).
    makeDicomByteString appends paddingChar ' ' when the length is odd
    (dcbytstr.cc). getPartialValue copies getLength bytes of that value
    (dcelem.cc). No HTML encoding is applied.
    """
    raw = value.encode("utf-8")
    if len(raw) % 2 == 1:
        raw += b" "
    return raw


def styled_json(value):
    """JsonCpp 0.6.0-rc2 StyledWriter for the values this stub emits.

    Object keys are std::map order (strcmp). Indent is 3. A colon is
    surrounded by spaces. The document ends with a newline.
    """

    def emit(item, indent):
        if item is None:
            return "null"
        if isinstance(item, bool):
            return "true" if item else "false"
        if isinstance(item, int):
            return str(item)
        if isinstance(item, str):
            escaped = item.replace("\\", "\\\\").replace('"', '\\"')
            return '"' + escaped + '"'
        if isinstance(item, list):
            if not item:
                return "[]"
            multiline = any(isinstance(child, (dict, list)) and len(child) > 0 for child in item)
            if not multiline:
                line = "[ " + ", ".join(emit(child, indent) for child in item) + " ]"
                if len(line) < 74:
                    return line
            inner = " " * (indent + 3)
            rows = [inner + emit(child, indent + 3) for child in item]
            return "[\n" + ",\n".join(rows) + "\n" + (" " * indent) + "]"
        if isinstance(item, dict):
            if not item:
                return "{}"
            inner = " " * (indent + 3)
            rows = []
            for key in sorted(item):
                rows.append(inner + emit(key, indent + 3) + " : " + emit(item[key], indent + 3))
            return "{\n" + ",\n".join(rows) + "\n" + (" " * indent) + "}"
        raise TypeError(type(item))

    return emit(value, 0) + "\n"


class Store:
    def __init__(self):
        self.lock = threading.Lock()
        self.instances = {}
        self.series = {}
        self.next_id = 0

    def new_id(self):
        self.next_id += 1
        n = f"{self.next_id:08x}"
        return f"{n}-{n}-{n}-{n}-{n}"

    def add_instance(self, tags, series_id=None):
        with self.lock:
            instance_id = self.new_id()
            if series_id is None:
                series_id = self.new_id()
            file_uuid = self.new_id()
            self.instances[instance_id] = {
                "tags": dict(tags),
                "series_id": series_id,
                "file_uuid": file_uuid,
                "file_size": 128,
            }
            self.series.setdefault(series_id, [])
            if instance_id not in self.series[series_id]:
                self.series[series_id].append(instance_id)
            return instance_id, series_id

    def instance_json(self, instance_id):
        inst = self.instances[instance_id]
        # Field set from ServerIndex::LookupResource for an instance.
        # IndexInSeries is null when the instance has no Instance Number
        # (ServerIndex.cpp).
        return {
            "FileSize": inst["file_size"],
            "FileUuid": inst["file_uuid"],
            "ID": instance_id,
            # ServerIndex stores Instance Number as IndexInSeries and writes
            # that integer. With no instance number the field is JSON null,
            # and viewer.php then divides the string "null" (a TypeError on
            # PHP 8 before the tag echo).
            "IndexInSeries": index_in_series(inst["tags"]),
            "MainDicomTags": {},
            "Type": "Instance",
        }


STORE = Store()


def reset_store():
    global STORE
    STORE = Store()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        return

    def _body(self):
        length = int(self.headers.get("Content-Length", "0") or "0")
        return self.rfile.read(length) if length else b""

    def _send(self, code, body, content_type):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _stored_answer(self, instance_id):
        # OrthancRestApi::AnswerStoredInstance + StyledWriter.
        payload = {
            "ID": instance_id,
            "Path": "/instances/" + instance_id,
            "Status": "Success",
        }
        self._send(200, styled_json(payload), "application/json")

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        parts = [p for p in path.split("/") if p]
        if parts == ["series"]:
            with STORE.lock:
                ids = list(STORE.series.keys())
            self._send(200, styled_json(ids), "application/json")
            return
        if len(parts) == 3 and parts[0] == "series" and parts[2] == "instances":
            series_id = parts[1]
            with STORE.lock:
                ids = list(STORE.series.get(series_id, []))
                items = [STORE.instance_json(i) for i in ids]
            self._send(200, styled_json(items), "application/json")
            return
        if len(parts) == 3 and parts[0] == "instances" and parts[2] == "image-uint16":
            if parts[1] not in STORE.instances:
                self._send(404, b"", "text/plain")
                return
            self._send(200, MINIMAL_PNG, "image/png")
            return
        if len(parts) == 4 and parts[0] == "instances" and parts[2] == "content":
            inst = STORE.instances.get(parts[1])
            if inst is None:
                self._send(404, b"", "text/plain")
                return
            raw = inst["tags"].get(parts[3], "")
            self._send(200, dicom_string_bytes(raw), "application/octet-stream")
            return
        self._send(404, b"", "text/plain")

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        body = self._body()
        parts = [p for p in path.split("/") if p]
        if parts == ["tools", "create-dicom"]:
            # ParseJsonRequest reads the body as JSON. ParseReplacements copies
            # each string into DicomModification::Replace unchanged.
            try:
                request = json.loads(body.decode("utf-8"))
            except json.JSONDecodeError:
                self._send(400, b"", "text/plain")
                return
            tags = {}
            for name, value in request.items():
                if name == "PixelData" or not isinstance(value, str):
                    continue
                tag = TAG_BY_NAME.get(name)
                if tag:
                    tags[tag] = value
            instance_id, _series_id = STORE.add_instance(tags)
            self._stored_answer(instance_id)
            return
        if len(parts) == 3 and parts[0] == "instances" and parts[2] == "modify":
            # AnonymizeOrModifyInstance applies Replace then returns the file
            # bytes. It does not store the modified instance.
            original = STORE.instances.get(parts[1])
            if original is None:
                self._send(404, b"", "text/plain")
                return
            request = json.loads(body.decode("utf-8"))
            tags = dict(original["tags"])
            for name, value in request.get("Replace", {}).items():
                tag = TAG_BY_NAME.get(name)
                if tag and isinstance(value, str):
                    tags[tag] = value
            payload = {
                "tags": tags,
                "series_id": original["series_id"],
            }
            raw = b"MODIFIED\n" + json.dumps(payload).encode("utf-8")
            self._send(200, raw, "application/octet-stream")
            return
        if parts == ["instances"]:
            # UploadDicomFile stores the posted instance. The bytes here are
            # the modify response, whose tag strings were already replaced.
            if body.startswith(b"MODIFIED\n"):
                payload = json.loads(body[len(b"MODIFIED\n"):].decode("utf-8"))
                instance_id, _series_id = STORE.add_instance(payload["tags"], payload["series_id"])
                self._stored_answer(instance_id)
                return
            self._send(400, b"", "text/plain")
            return
        self._send(404, b"", "text/plain")

    def do_DELETE(self):
        parts = [p for p in self.path.split("/") if p]
        if len(parts) == 2 and parts[0] == "instances":
            instance_id = parts[1]
            with STORE.lock:
                inst = STORE.instances.pop(instance_id, None)
                if inst is not None:
                    series = STORE.series.get(inst["series_id"], [])
                    STORE.series[inst["series_id"]] = [i for i in series if i != instance_id]
            self._send(200, styled_json({}), "application/json")
            return
        self._send(404, b"", "text/plain")


def serve(port=8042):
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server
