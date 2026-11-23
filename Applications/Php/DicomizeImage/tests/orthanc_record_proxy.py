#!/usr/bin/env python3
"""Forward localhost:8042 to a real Orthanc and record each request body.

createDicom.php posts to http://localhost:8042 with no seam for a test double.
This process is only a byte pipe: it does not interpret DICOM or JSON.
Orthanc on the backend port is what accepts or stores the instance.
"""

import json
import socket
import sys
import threading

BACKEND_HOST = "127.0.0.1"
BACKEND_PORT = 18042
LISTEN_HOST = "127.0.0.1"
LISTEN_PORT = 8042


def read_until(sock, buf, marker):
    while marker not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            return None
        buf += chunk
    return buf


def header_value(head, name):
    prefix = name.lower() + ":"
    for line in head.split("\r\n")[1:]:
        if line.lower().startswith(prefix):
            return line.split(":", 1)[1].strip()
    return None


def read_message(sock, buf):
    buf = read_until(sock, buf, b"\r\n\r\n")
    if buf is None:
        return None, None, b""
    raw_head, buf = buf.split(b"\r\n\r\n", 1)
    head = raw_head.decode("iso-8859-1")
    length_text = header_value(head, "content-length")
    if length_text is None:
        # No length and no chunked body in the requests this script sends.
        return head, b"", buf
    length = int(length_text)
    while len(buf) < length:
        chunk = sock.recv(65536)
        if not chunk:
            break
        buf += chunk
    return head, buf[:length], buf[length:]


def expects_continue(head):
    expect = header_value(head, "expect")
    return expect is not None and "100-continue" in expect.lower()


def read_request(sock, buf):
    buf = read_until(sock, buf, b"\r\n\r\n")
    if buf is None:
        return None, None, b""
    raw_head, buf = buf.split(b"\r\n\r\n", 1)
    head = raw_head.decode("iso-8859-1")
    length_text = header_value(head, "content-length")
    length = int(length_text) if length_text is not None else 0
    if expects_continue(head) and len(buf) < length:
        sock.sendall(b"HTTP/1.1 100 Continue\r\n\r\n")
    while len(buf) < length:
        chunk = sock.recv(65536)
        if not chunk:
            break
        buf += chunk
    return head, buf[:length], buf[length:]


def forward(head, body):
    lines = head.split("\r\n")
    filtered = []
    for index, line in enumerate(lines):
        if index > 0 and line.lower().startswith("expect:"):
            continue
        filtered.append(line)
    request = ("\r\n".join(filtered) + "\r\n\r\n").encode("iso-8859-1") + body
    backend = socket.create_connection((BACKEND_HOST, BACKEND_PORT))
    backend.sendall(request)
    response_head, response_body, _extra = read_message(backend, b"")
    backend.close()
    if response_head is None:
        raise RuntimeError("backend closed before a response")
    raw = (response_head + "\r\n\r\n").encode("iso-8859-1") + response_body
    return raw


def handle(client, log_path, log_lock):
    buf = b""
    try:
        while True:
            head, body, buf = read_request(client, buf)
            if head is None:
                break
            first = head.split("\r\n", 1)[0]
            parts = first.split(" ")
            method = parts[0] if parts else ""
            path = parts[1] if len(parts) > 1 else ""
            record = {
                "method": method,
                "path": path.split("?", 1)[0],
                "body": body.decode("utf-8", "replace"),
            }
            line = json.dumps(record, ensure_ascii=False) + "\n"
            with log_lock:
                with open(log_path, "a", encoding="utf-8") as log:
                    log.write(line)
                    log.flush()
            client.sendall(forward(head, body))
            if header_value(head, "connection") and "close" in header_value(head, "connection").lower():
                break
    except Exception as exc:
        sys.stderr.write("proxy request failed: %s\n" % exc)
    finally:
        client.close()


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: orthanc_record_proxy.py LOGFILE\n")
        return 2
    log_path = sys.argv[1]
    log_lock = threading.Lock()
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((LISTEN_HOST, LISTEN_PORT))
    server.listen(32)
    sys.stdout.write("proxy listening on %s:%s -> %s:%s\n" % (LISTEN_HOST, LISTEN_PORT, BACKEND_HOST, BACKEND_PORT))
    sys.stdout.flush()
    while True:
        client, _addr = server.accept()
        threading.Thread(target=handle, args=(client, log_path, log_lock), daemon=True).start()


if __name__ == "__main__":
    sys.exit(main() or 0)
