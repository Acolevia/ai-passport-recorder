from __future__ import annotations

import http.client
import importlib.util
import json
import tempfile
import threading
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("server.py")
SPEC = importlib.util.spec_from_file_location("passport_server", MODULE_PATH)
assert SPEC and SPEC.loader
server_module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(server_module)


class ServerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        server_module.DATA_ROOT = Path(self.temporary.name)
        server_module.UPLOAD_TOKEN = "test-secret"
        self.server = server_module.ThreadingHTTPServer(
            ("127.0.0.1", 0), server_module.RecorderHandler
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port)

    def tearDown(self) -> None:
        self.connection.close()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.temporary.cleanup()

    @property
    def authorization(self) -> dict[str, str]:
        return {"Authorization": "Bearer test-secret"}

    def request(self, method: str, path: str, body: bytes = b"", **headers: str):
        request_headers = {**self.authorization, **headers}
        self.connection.request(method, path, body=body, headers=request_headers)
        return self.connection.getresponse()

    def test_health_does_not_require_authentication(self) -> None:
        self.connection.request("GET", "/health")
        response = self.connection.getresponse()
        self.assertEqual(response.status, 200)
        self.assertEqual(json.loads(response.read())["status"], "ok")

        response = self.request("GET", "/api/v1/status")
        self.assertEqual(response.status, 200)
        self.assertTrue(json.loads(response.read())["authenticated"])

    def test_resumes_and_completes_upload(self) -> None:
        path = "/api/v1/uploads/DEVICE01/R0000001"
        response = self.request(
            "PUT", path, b"abc", **{"Upload-Offset": "0", "Upload-Length": "6"}
        )
        self.assertEqual(response.status, 204)
        self.assertEqual(response.getheader("Upload-Offset"), "3")
        response.read()

        response = self.request("HEAD", path)
        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("Upload-Offset"), "3")
        response.read()

        response = self.request(
            "PUT",
            path,
            b"def",
            **{
                "Upload-Offset": "3",
                "Upload-Length": "6",
                "X-Sample-Count": "320",
                "X-Sample-Rate": "16000",
                "X-Codec": "opus-framed-v2",
            },
        )
        self.assertEqual(response.status, 201)
        response.read()
        self.assertEqual(
            (server_module.DATA_ROOT / "DEVICE01" / "R0000001.frc").read_bytes(), b"abcdef"
        )

        response = self.request("HEAD", path)
        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("Upload-Complete"), "1")
        self.assertIsNone(response.getheader("Upload-Offset"))
        response.read()

    def test_rejects_wrong_offset_and_bad_token(self) -> None:
        path = "/api/v1/uploads/DEVICE01/R0000002"
        response = self.request(
            "PUT", path, b"x", **{"Upload-Offset": "1", "Upload-Length": "2"}
        )
        self.assertEqual(response.status, 409)
        self.assertEqual(response.getheader("Upload-Offset"), "0")
        response.read()

        self.connection.request("HEAD", path, headers={"Authorization": "Bearer wrong"})
        response = self.connection.getresponse()
        self.assertEqual(response.status, 401)
        response.read()


if __name__ == "__main__":
    unittest.main()
