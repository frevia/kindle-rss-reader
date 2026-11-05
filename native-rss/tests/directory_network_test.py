#!/usr/bin/env python3
"""Exercise directory refresh and cache safety against a loopback HTTP server."""
import http.server
import pathlib
import subprocess
import sys
import threading
import time
import unittest


OPML = b'''<?xml version="1.0"?><opml version="2.0"><body><outline text="Tech">
<outline text="First &amp; &lt;Second&gt;" title="First &amp; &lt;Second&gt;" xmlUrl="http://LOCALHOST:PORT/feed#one"/>
<outline text="Duplicate" xmlUrl="http://localhost:PORT/feed#two"/>
<outline text="Credentials" xmlUrl="http://user:pass@example.com/feed"/>
</outline></body></opml>'''


class DirectoryHandler(http.server.BaseHTTPRequestHandler):
    sequence = {}
    lock = threading.Lock()
    request_count = 0

    def log_message(self, *_args):
        pass

    def do_GET(self):
        with self.lock:
            type(self).request_count += 1
        if self.path in ("/switch.opml", "/cancel-sequence.opml", "/limit-sequence.opml"):
            with self.lock:
                count = self.sequence.get(self.path, 0)
                self.sequence[self.path] = count + 1
            if self.path == "/limit-sequence.opml" and count:
                outlines = b''.join(b'<outline xmlUrl="https://example.com/' + str(i).encode() + b'"/>' for i in range(50000))
                body, status = b'<opml><body>' + outlines + b'</body></opml>', 200
            elif self.path == "/cancel-sequence.opml" and count:
                time.sleep(2)
                body, status = OPML, 200
            elif count:
                body, status = b"<html>bad</html>", 200
            else:
                body, status = OPML, 200
        elif self.path == "/ok.opml":
            body, status = OPML, 200
        elif self.path == "/large-cache.opml":
            outlines = b"".join(
                b'<outline text="Feed ' + str(i).encode() + b' ' + b"x" * 100 + b'" xmlUrl="https://large.example/feed/' + str(i).encode() + b'"/>'
                for i in range(3200)
            )
            body, status = b"<opml><body>" + outlines + b"</body></opml>", 200
        elif self.path == "/html.opml":
            body, status = b"<html><body>not OPML</body></html>", 200
        elif self.path == "/malformed.opml":
            body, status = b"<opml><body>", 200
        elif self.path == "/dtd.opml":
            body, status = b'<!DOCTYPE opml [<!ENTITY x "boom">]><opml><body>&x;</body></opml>', 200
        else:
            body, status = b"server error", 500
        body = body.replace(b"PORT", str(self.server.server_port).encode())
        self.send_response(status)
        self.send_header("Content-Type", "application/xml")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


class DirectoryNetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), DirectoryHandler)
        cls.server.daemon_threads = True
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.server.server_port}"
        cls.binary = str(pathlib.Path(sys.argv[1]).resolve())

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def run_case(self, scenario):
        return subprocess.run([self.binary, self.base, scenario], text=True, capture_output=True, timeout=15)

    def test_success_deduplicates_sanitizes_and_reads_cache_offline(self):
        result = self.run_case("success")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cache_payload_may_exceed_metadata_limit(self):
        result = self.run_case("large-cache")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_failures_do_not_write_cache(self):
        for scenario in ("html", "malformed", "dtd", "error"):
            with self.subTest(scenario=scenario):
                result = self.run_case(scenario)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_failed_refresh_and_cancellation_keep_previous_cache(self):
        for scenario in ("preserve", "limit", "cancel"):
            with self.subTest(scenario=scenario):
                result = self.run_case(scenario)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_pre_cancel_and_website_are_rejected(self):
        for scenario in ("pre-cancel", "website"):
            with self.subTest(scenario=scenario):
                before = DirectoryHandler.request_count
                result = self.run_case(scenario)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if scenario == "website":
                    self.assertEqual(DirectoryHandler.request_count, before)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
