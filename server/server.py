#!/usr/bin/env python3
"""Small resumable upload server for AI Passport Recorder devices."""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import threading
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

DATA_ROOT = Path(os.environ.get("AI_PASSPORT_DATA_DIR", "/data")).resolve()
UPLOAD_TOKEN = os.environ.get("AI_PASSPORT_UPLOAD_TOKEN", "")
MAX_UPLOAD_BYTES = int(os.environ.get("AI_PASSPORT_MAX_UPLOAD_BYTES", str(64 * 1024 * 1024)))
PORT = int(os.environ.get("AI_PASSPORT_PORT", "8080"))
IDENTIFIER = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
_locks_guard = threading.Lock()
_upload_locks: dict[str, threading.Lock] = {}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def upload_lock(key: str) -> threading.Lock:
    with _locks_guard:
        return _upload_locks.setdefault(key, threading.Lock())


def atomic_json(path: Path, value: dict[str, object]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, path)


class RecorderHandler(BaseHTTPRequestHandler):
    server_version = "AIPassportRecorder/1.0"
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path == "/health":
            self.send_json(HTTPStatus.OK, {"status": "ok", "service": "ai-passport-recorder"})
            return
        if not self.authorized():
            return
        if parsed.path == "/api/v1/status":
            self.send_json(HTTPStatus.OK, {"status": "ok", "authenticated": True})
            return
        if parsed.path == "/api/v1/recordings":
            self.list_recordings()
            return
        parts = self.upload_parts(parsed.path, prefix="/api/v1/recordings/")
        if parts is not None:
            self.download_recording(*parts)
            return
        self.send_json(HTTPStatus.NOT_FOUND, {"error": "not_found"})

    def do_HEAD(self) -> None:  # noqa: N802
        if not self.authorized():
            return
        parts = self.upload_parts(urlparse(self.path).path, prefix="/api/v1/uploads/")
        if parts is None:
            self.send_empty(HTTPStatus.NOT_FOUND)
            return
        device_id, recording_id = parts
        directory = DATA_ROOT / device_id
        final_path = directory / f"{recording_id}.frc"
        partial_path = directory / f"{recording_id}.part"
        if final_path.is_file():
            self.send_empty(HTTPStatus.OK, {"Upload-Complete": "1"})
        elif partial_path.is_file():
            self.send_empty(
                HTTPStatus.OK,
                {"Upload-Offset": str(partial_path.stat().st_size), "Upload-Complete": "0"},
            )
        else:
            self.send_empty(HTTPStatus.NOT_FOUND, {"Upload-Offset": "0"})

    def do_PUT(self) -> None:  # noqa: N802
        if not self.authorized():
            return
        parts = self.upload_parts(urlparse(self.path).path, prefix="/api/v1/uploads/")
        if parts is None:
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "not_found"})
            return
        try:
            offset = self.integer_header("Upload-Offset", minimum=0)
            total = self.integer_header("Upload-Length", minimum=1, maximum=MAX_UPLOAD_BYTES)
            content_length = self.integer_header("Content-Length", minimum=0)
        except ValueError as error:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        if content_length > total - offset:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": "body_exceeds_upload_length"})
            return
        self.receive_upload(parts[0], parts[1], offset, total, content_length)

    def authorized(self) -> bool:
        if not UPLOAD_TOKEN:
            self.send_json(
                HTTPStatus.SERVICE_UNAVAILABLE,
                {"error": "AI_PASSPORT_UPLOAD_TOKEN is not configured"},
            )
            return False
        supplied = self.headers.get("Authorization", "")
        expected = f"Bearer {UPLOAD_TOKEN}"
        if not hmac.compare_digest(supplied, expected):
            self.send_empty(HTTPStatus.UNAUTHORIZED, {"WWW-Authenticate": "Bearer"})
            return False
        return True

    @staticmethod
    def upload_parts(path: str, prefix: str) -> tuple[str, str] | None:
        if not path.startswith(prefix):
            return None
        parts = [unquote(part) for part in path[len(prefix) :].split("/")]
        if len(parts) != 2 or not all(IDENTIFIER.fullmatch(part) for part in parts):
            return None
        return parts[0], parts[1]

    def integer_header(self, name: str, minimum: int, maximum: int | None = None) -> int:
        try:
            value = int(self.headers.get(name, ""), 10)
        except ValueError as error:
            raise ValueError(f"invalid_{name.lower()}") from error
        if value < minimum or (maximum is not None and value > maximum):
            raise ValueError(f"invalid_{name.lower()}")
        return value

    def receive_upload(
        self, device_id: str, recording_id: str, offset: int, total: int, body_size: int
    ) -> None:
        directory = DATA_ROOT / device_id
        directory.mkdir(parents=True, exist_ok=True)
        final_path = directory / f"{recording_id}.frc"
        partial_path = directory / f"{recording_id}.part"
        key = f"{device_id}/{recording_id}"
        with upload_lock(key):
            if final_path.is_file():
                self.discard_body(body_size)
                if final_path.stat().st_size == total:
                    self.send_empty(HTTPStatus.NO_CONTENT, {"Upload-Complete": "1"})
                else:
                    self.send_json(HTTPStatus.CONFLICT, {"error": "completed_size_mismatch"})
                return
            current = partial_path.stat().st_size if partial_path.exists() else 0
            if current != offset:
                self.discard_body(body_size)
                self.send_empty(HTTPStatus.CONFLICT, {"Upload-Offset": str(current)})
                return

            remaining = body_size
            received = 0
            with partial_path.open("ab") as output:
                while remaining > 0:
                    block = self.rfile.read(min(65536, remaining))
                    if not block:
                        break
                    output.write(block)
                    received += len(block)
                    remaining -= len(block)
                output.flush()
                os.fsync(output.fileno())
            new_offset = current + received
            if remaining != 0:
                self.close_connection = True
                return
            if new_offset == total:
                digest = hashlib.sha256()
                with partial_path.open("rb") as completed:
                    while block := completed.read(65536):
                        digest.update(block)
                sha256 = digest.hexdigest()
                os.replace(partial_path, final_path)
                metadata = {
                    "device_id": device_id,
                    "recording_id": recording_id,
                    "bytes": total,
                    "sha256": sha256,
                    "sample_count": self.headers.get("X-Sample-Count", ""),
                    "sample_rate": self.headers.get("X-Sample-Rate", ""),
                    "codec": self.headers.get("X-Codec", ""),
                    "completed_at": utc_now(),
                }
                atomic_json(directory / f"{recording_id}.json", metadata)
                self.send_empty(
                    HTTPStatus.CREATED,
                    {"Upload-Offset": str(new_offset), "Upload-Complete": "1"},
                )
            else:
                self.send_empty(
                    HTTPStatus.NO_CONTENT,
                    {"Upload-Offset": str(new_offset), "Upload-Complete": "0"},
                )

    def discard_body(self, length: int) -> None:
        remaining = length
        while remaining > 0:
            block = self.rfile.read(min(65536, remaining))
            if not block:
                return
            remaining -= len(block)

    def list_recordings(self) -> None:
        recordings: list[dict[str, object]] = []
        if DATA_ROOT.exists():
            for metadata_path in sorted(DATA_ROOT.glob("*/*.json"), reverse=True):
                try:
                    recordings.append(json.loads(metadata_path.read_text(encoding="utf-8")))
                except (OSError, json.JSONDecodeError):
                    continue
        self.send_json(HTTPStatus.OK, {"recordings": recordings})

    def download_recording(self, device_id: str, recording_id: str) -> None:
        path = DATA_ROOT / device_id / f"{recording_id}.frc"
        if not path.is_file():
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "not_found"})
            return
        size = path.stat().st_size
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Disposition", f'attachment; filename="{recording_id}.frc"')
        self.send_header("Content-Length", str(size))
        self.end_headers()
        with path.open("rb") as source:
            while block := source.read(65536):
                self.wfile.write(block)

    def send_json(self, status: HTTPStatus, value: dict[str, object]) -> None:
        body = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def send_empty(self, status: HTTPStatus, headers: dict[str, str] | None = None) -> None:
        self.send_response(status)
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, format: str, *args: object) -> None:
        print(f"{self.address_string()} - {format % args}", flush=True)


def main() -> None:
    DATA_ROOT.mkdir(parents=True, exist_ok=True)
    if not UPLOAD_TOKEN:
        raise SystemExit("AI_PASSPORT_UPLOAD_TOKEN must be set")
    server = ThreadingHTTPServer(("0.0.0.0", PORT), RecorderHandler)
    print(f"AI Passport Recorder server listening on :{PORT}, data={DATA_ROOT}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
