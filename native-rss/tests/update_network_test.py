"""Exercise the real GitHub HTTPS download path through a local TLS proxy."""
import hashlib
from http.server import BaseHTTPRequestHandler
import json
import os
from pathlib import Path
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest

from update_test import VERSION, make_zip, payload


class Proxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class Connect(socketserver.BaseRequestHandler):
    def handle(self):
        stream = self.request.makefile("rb")
        first = stream.readline(8192)
        if not first.startswith(b"CONNECT "):
            return
        while stream.readline(8192) not in (b"\r\n", b""):
            pass
        self.request.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
        with self.server.tls.wrap_socket(self.request, server_side=True) as connection:
            Http(connection, self.client_address, self.server)


class Http(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.seen.append(self.path)
        mode = self.server.mode
        if self.path.endswith("SHA256SUMS.txt"):
            digest = "0" * 64 if mode == "bad-sha" else self.server.digest
            body = f"{digest}  RSS-Reader-{VERSION}-KPW3.zip\n".encode()
        elif self.path.endswith("-KPW3.zip"):
            self.send_response(302)
            self.send_header("Location", "http://github.com/unsafe" if mode == "downgrade" else
                             "https://evil.invalid/payload" if mode == "foreign-host" else
                             "https://release-assets.githubusercontent.com/fixture.zip")
            self.end_headers()
            self.wfile.write(b"redirect body must not contaminate the ZIP")
            return
        elif self.path == "/fixture.zip":
            body = self.server.package
            if mode == "incomplete":
                body = body[:-7]
        else:
            self.send_error(404)
            return
        self.send_response(429 if mode == "rate-limit" else 200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class UpdateNetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.base = Path(cls.temp.name)
        cert, key = cls.base / "cert.pem", cls.base / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-keyout", str(key), "-out", str(cert), "-subj", "/CN=github.com",
                        "-addext", "subjectAltName=DNS:github.com,DNS:release-assets.githubusercontent.com"],
                       check=True, capture_output=True)
        cls.proxy = Proxy(("127.0.0.1", 0), Connect)
        cls.addClassCleanup(cls.proxy.server_close)
        cls.proxy.tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cls.proxy.tls.load_cert_chain(cert, key)
        archive = cls.base / "package.zip"
        make_zip(archive, payload())
        cls.proxy.package = archive.read_bytes()
        cls.proxy.digest = hashlib.sha256(cls.proxy.package).hexdigest()
        cls.proxy.mode = "normal"
        cls.proxy.seen = []
        cls.thread = threading.Thread(target=cls.proxy.serve_forever, daemon=True)
        cls.thread.start()
        cls.addClassCleanup(cls.proxy.shutdown)
        cls.env = dict(os.environ, HTTPS_PROXY=f"http://127.0.0.1:{cls.proxy.server_address[1]}",
                       HTTP_PROXY="", ALL_PROXY="", NO_PROXY="", RSS_CA_BUNDLE=str(cert))

    def test_download_follows_only_github_https_and_checks_actual_archive(self):
        for mode in ("normal", "bad-sha", "downgrade", "foreign-host", "incomplete", "rate-limit"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "native-rss/bin").mkdir(parents=True)
                old = root / "native-rss/bin/kindle-rss"
                old.write_bytes(b"old binary fixture")
                release = {"tag_name": VERSION, "draft": False, "prerelease": False, "assets": [
                    {"name": f"RSS-Reader-{VERSION}-KPW3.zip", "size": len(self.proxy.package),
                     "browser_download_url": f"https://github.com/frevia/kindle-rss-reader/releases/download/{VERSION}/RSS-Reader-{VERSION}-KPW3.zip"},
                    {"name": "SHA256SUMS.txt", "size": 100,
                     "browser_download_url": f"https://github.com/frevia/kindle-rss-reader/releases/download/{VERSION}/SHA256SUMS.txt"}]}
                metadata = root / "release.json"
                metadata.write_text(json.dumps(release))
                self.proxy.mode = mode
                self.proxy.seen.clear()
                result = subprocess.run([DRIVER, "prepare", str(root), str(metadata)], env=self.env,
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode == 0, mode == "normal", result.stderr)
                folder = root / "native-rss/.update"
                self.assertEqual((folder / "ready").exists(), mode == "normal")
                self.assertEqual(old.read_bytes(), b"old binary fixture")
                self.assertFalse((folder / "package.zip").exists())
                if mode == "normal":
                    self.assertEqual((folder / "runner").read_bytes(), old.read_bytes())
                    self.assertIn("/fixture.zip", self.proxy.seen)
                elif mode in ("downgrade", "foreign-host"):
                    self.assertNotIn("/fixture.zip", self.proxy.seen)


if __name__ == "__main__":
    DRIVER = str(Path(sys.argv.pop()).resolve())
    unittest.main()
