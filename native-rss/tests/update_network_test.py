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
import time
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
        if self.path.startswith("/repos/frevia/kindle-rss-reader/releases"):
            body = json.dumps(self.server.release_list if "?" in self.path else self.server.release_list[0]).encode()
        elif self.path.endswith("SHA256SUMS.txt"):
            digest = "0" * 64 if mode == "bad-sha" else self.server.digest
            body = f"{digest}  RSS-Reader-{VERSION}-Kindle.zip\n".encode()
        elif self.path.endswith("-Kindle.zip"):
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
        if mode in ("slow-check", "cancel-check") and self.path.startswith("/repos/"):
            self.wfile.write(body[:1])
            self.wfile.flush()
            time.sleep(8 if mode == "cancel-check" else 3)
            body = body[1:]
        if mode == "slow" and self.path == "/fixture.zip":
            self.wfile.write(body[:512])
            self.wfile.flush()
            time.sleep(3)
            body = body[512:]
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
                        "-addext", "subjectAltName=DNS:github.com,DNS:api.github.com,DNS:release-assets.githubusercontent.com"],
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

    def test_check_uses_the_installed_channel_and_selects_newest_version(self):
        current = subprocess.check_output([DRIVER, "version"], text=True).strip()
        beta = "-beta." in current
        def release(tag, prerelease=False):
            return {"tag_name": tag, "draft": False, "prerelease": prerelease, "assets": [
                {"name": f"RSS-Reader-{tag}-Kindle.zip", "size": 123,
                 "browser_download_url": f"https://github.com/frevia/kindle-rss-reader/releases/download/{tag}/RSS-Reader-{tag}-Kindle.zip"},
                {"name": "SHA256SUMS.txt", "browser_download_url": f"https://github.com/frevia/kindle-rss-reader/releases/download/{tag}/SHA256SUMS.txt"}]}
        self.proxy.release_list = [release("v998.0.0"), release("v999.0.0-beta.2", True),
                                   release("v999.0.0-beta.10", True)]
        self.proxy.mode = "normal"
        self.proxy.seen.clear()
        result = subprocess.run([DRIVER, "check"], env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "v999.0.0-beta.10 1" if beta else "v998.0.0 1")
        self.assertEqual(self.proxy.seen, ["/repos/frevia/kindle-rss-reader/releases?per_page=100" if beta else
                                          "/repos/frevia/kindle-rss-reader/releases/latest"])

    def test_check_reports_progress_while_response_is_delayed(self):
        current = subprocess.check_output([DRIVER, "version"], text=True).strip()
        self.proxy.release_list = [{"tag_name": "v999.0.0", "draft": False, "prerelease": False, "assets": []}]
        self.proxy.mode = "slow-check"
        result = subprocess.run([DRIVER, "check"], env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("检查更新", result.stderr)
        self.assertIn("已等待", result.stderr)

    def test_check_can_cancel_while_waiting_for_response(self):
        self.proxy.release_list = []
        self.proxy.mode = "cancel-check"
        started = time.monotonic()
        result = subprocess.run([DRIVER, "check", "cancel", "1800"], env=self.env,
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("更新已取消", result.stderr)
        self.assertLess(time.monotonic() - started, 3)

    def test_download_follows_only_github_https_and_checks_actual_archive(self):
        for mode in ("slow", "normal", "bad-sha", "downgrade", "foreign-host", "incomplete", "rate-limit"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "native-rss/bin").mkdir(parents=True)
                old = root / "native-rss/bin/kindle-rss"
                old.write_bytes(b"old binary fixture")
                release = {"tag_name": VERSION, "draft": False, "prerelease": False, "assets": [
                    {"name": f"RSS-Reader-{VERSION}-Kindle.zip", "size": len(self.proxy.package),
                     "browser_download_url": f"https://github.com/frevia/kindle-rss-reader/releases/download/{VERSION}/RSS-Reader-{VERSION}-Kindle.zip"},
                    {"name": "SHA256SUMS.txt", "size": 100,
                     "browser_download_url": f"https://github.com/frevia/kindle-rss-reader/releases/download/{VERSION}/SHA256SUMS.txt"}]}
                metadata = root / "release.json"
                metadata.write_text(json.dumps(release))
                self.proxy.mode = mode
                self.proxy.seen.clear()
                result = subprocess.run([DRIVER, "prepare", str(root), str(metadata)], env=self.env,
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode == 0, mode in ("normal", "slow"), result.stderr)
                if mode == "slow":
                    self.assertIn("512", result.stderr, "small slow downloads must report bytes before reaching 1 MB")
                folder = root / "native-rss/.update"
                self.assertEqual((folder / "ready").exists(), mode in ("normal", "slow"))
                self.assertEqual(old.read_bytes(), b"old binary fixture")
                self.assertFalse((folder / "package.zip").exists())
                if mode in ("normal", "slow"):
                    self.assertEqual((folder / "runner").read_bytes(), old.read_bytes())
                    self.assertIn("/fixture.zip", self.proxy.seen)
                elif mode in ("downgrade", "foreign-host"):
                    self.assertNotIn("/fixture.zip", self.proxy.seen)


if __name__ == "__main__":
    DRIVER = str(Path(sys.argv.pop()).resolve())
    unittest.main()
