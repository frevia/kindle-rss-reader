#!/usr/bin/env python3
"""Exercise selection translation requests against a local fake provider only."""
import http.server
import json
import pathlib
import subprocess
import sys
import threading
import time
import unittest


class Provider(http.server.BaseHTTPRequestHandler):
    seen = []
    lock = threading.Lock()

    def log_message(self, *_args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with self.lock:
            self.seen.append((self.path, dict(self.headers), request))
        if self.path == "/slow":
            time.sleep(1.5)
        status = 200
        response = {"choices": [{"message": {"role": "assistant", "content": "译文 & 原文"}, "finish_reason": "stop"}]}
        if self.path == "/401":
            status, response = 401, {"error": "fake failure"}
        elif self.path == "/html":
            response["choices"][0]["message"]["content"] = "<script>bad()</script>"
        elif self.path == "/truncated":
            response["choices"][0]["finish_reason"] = "length"
        body = json.dumps(response, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


class SelectionNetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
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

    def run_case(self, path, scenario, provider=None):
        return subprocess.run([self.binary, self.base + path, scenario] + ([provider] if provider else []), text=True,
                              capture_output=True, timeout=15)

    def requests(self):
        with Provider.lock:
            return list(Provider.seen)

    def assert_request(self, item):
        path, headers, request = item
        self.assertEqual(headers["Authorization"], "Bearer fake-selection-key")
        self.assertEqual(request["model"], "qwen-mt-flash")
        self.assertEqual(request["translation_options"], {"source_lang": "English", "target_lang": "Chinese"})
        self.assertEqual(request["messages"], [{"role": "user", "content": "Selected phrase."}])
        self.assertNotIn("Full article body", json.dumps(request))
        self.assertNotIn("<p>", json.dumps(request))

    def setUp(self):
        with Provider.lock:
            Provider.seen = []

    def test_plain_single_request_cache_and_html_safe_display_value(self):
        result = self.run_case("/ok", "success-cache")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "译文 & 原文")
        seen = self.requests()
        self.assertEqual(len(seen), 1)
        self.assert_request(seen[0])

    def test_provider_failures_do_not_cache(self):
        for endpoint in ("/401", "/html", "/truncated"):
            with self.subTest(endpoint=endpoint):
                with Provider.lock:
                    Provider.seen = []
                result = self.run_case(endpoint, "failure")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                seen = self.requests()
                self.assertEqual(len(seen), 1)
                self.assert_request(seen[0])

    def test_chat_provider_selection_is_manual_single_request_and_cached(self):
        for provider in ("siliconflow", "zhipu", "openai-compatible"):
            with self.subTest(provider=provider):
                with Provider.lock:
                    Provider.seen = []
                result = self.run_case("/ok", "success-cache", provider)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                seen = self.requests()
                self.assertEqual(len(seen), 1)
                _, headers, request = seen[0]
                self.assertEqual(headers["Authorization"], "Bearer fake-selection-key")
                self.assertEqual(request["model"], "custom-chat")
                self.assertNotIn("translation_options", request)
                self.assertEqual(len(request["messages"]), 1)
                content = request["messages"][0]["content"]
                self.assertIn("Simplified Chinese", content)
                self.assertTrue(content.endswith("\n\nSelected phrase."))
                self.assertNotIn("Full article body", content)

    def test_cancel_before_send_and_during_request(self):
        result = self.run_case("/ok", "cancel-before")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.requests(), [])
        result = self.run_case("/slow", "cancel")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        seen = self.requests()
        self.assertEqual(len(seen), 1)
        self.assert_request(seen[0])

    def test_stale_source_does_not_request(self):
        result = self.run_case("/ok", "stale")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.requests(), [])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
