#!/usr/bin/env python3
"""Exercise Qwen HTTP contracts against a local fake provider, with no real key."""
import http.server
import json
import os
import pathlib
import re
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest


class Provider(http.server.BaseHTTPRequestHandler):
    seen = []
    lock = threading.Lock()

    def log_message(self, *_args):
        pass

    def do_POST(self):
        data = self.rfile.read(int(self.headers["Content-Length"]))
        with self.lock:
            self.seen.append((self.path, dict(self.headers), json.loads(data)))
            count = sum(path == self.path for path, _, _ in self.seen)
        status = 200
        request = json.loads(data)
        text = request["messages"][0]["content"]
        # Catch any accidental return to HTML-fragment requests with the old malformed-link failure.
        translated = "< a href=\"https://example.invalid/lost\">漏链接</a>" if re.search(r"<\s*/?\w", text) else text
        for source, target in (("English title", "英文标题"), ("quiet", "安静"),
                               ("reader", "读者"), ("Offline reading matters.", "离线阅读很重要。"),
                               ("Visit", "访问"), ("first source", "第一链接"),
                               ("then", "然后"), ("read", "阅读"),
                               ("second source", "第二链接"), ("Complex links", "复杂链接")):
            translated = translated.replace(source, target)
        response = {"choices": [{"message": {"role": "assistant", "content": translated}, "finish_reason": "stop"}]}
        headers = {}
        if self.path == "/slow":
            time.sleep(1.5)
        if self.path in ("/unauthorized", "/rate", "/error") or (self.path == "/partial" and count == 2) or (self.path == "/partial-node" and count == 3):
            status = {"/unauthorized": 401, "/rate": 429}.get(self.path, 500)
            response = {"message": "do not expose test-key in provider errors"}
        elif self.path == "/redirect":
            status = 307
            headers["Location"] = "/stolen"
        elif self.path == "/truncated":
            response["choices"][0]["finish_reason"] = "length"
        elif self.path == "/hostile":
            response["choices"][0]["message"]["content"] = "<script>bad()</script>译文"
        elif self.path == "/empty":
            response["choices"][0]["message"]["content"] = ""
        body = b"not JSON" if self.path == "/bad-json" else json.dumps(response).encode()
        if self.path == "/large":
            body = b"x" * (256 * 1024 + 1)
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        for key, value in headers.items():
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


class TranslationNetworkTests(unittest.TestCase):
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

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.db = pathlib.Path(self.folder.name) / "rss.sqlite"
        self.endpoint = "/ok"
        self.cli("list")
        self.html = "<p>A <strong>quiet</strong> reader.</p><p>Offline reading matters.</p><p>Use <code>keep_inline()</code> reader.</p><pre>print('keep code')</pre>"
        with sqlite3.connect(self.db) as conn:
            conn.execute("PRAGMA foreign_keys=ON")
            conn.execute("INSERT INTO feeds(id,url,title) VALUES(1,'https://example.invalid/feed','Example')")
            conn.execute("INSERT INTO entries(id,feed_id,guid,title,url,html,text) VALUES(1,1,'one','English title','https://example.invalid/story',?,?)",
                         (self.html, "A quiet reader.\nOffline reading matters.\nUse keep_inline() reader.\nprint('keep code')"))
        with Provider.lock:
            Provider.seen = []

    def tearDown(self):
        self.folder.cleanup()

    def cli(self, *args, expected=0, key=True):
        env = dict(os.environ)
        env["RSS_QWEN_API_KEY"] = "test-key" if key else ""
        env["RSS_QWEN_ENDPOINT"] = self.base + self.endpoint
        result = subprocess.run([self.binary, "--db", str(self.db), *args], env=env,
                                text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertNotIn("test-key", result.stdout + result.stderr)
        return result

    def rows(self, sql):
        with sqlite3.connect(self.db) as conn:
            return conn.execute(sql).fetchall()

    def assert_plain_requests(self, requests):
        for _, _, request in requests:
            self.assertEqual(request["translation_options"], {"source_lang": "English", "target_lang": "Chinese"})
            self.assertEqual(len(request["messages"]), 1)
            content = request["messages"][0]["content"]
            self.assertNotRegex(content, r"<\s*/?\w")
            self.assertNotRegex(content, r"https?://|mailto:")
            self.assertNotIn("RSSKEEP", content)
            self.assertNotIn("print('keep code')", content)
            self.assertNotIn("keep_inline()", content)

    def test_request_contract_cache_and_original_preservation(self):
        self.cli("translate", "1")
        first = len(Provider.seen)
        self.assertGreaterEqual(first, 3)
        for path, headers, request in Provider.seen:
            self.assertEqual(path, "/ok")
            self.assertEqual(headers["Authorization"], "Bearer test-key")
            self.assertEqual(request["model"], "qwen-mt-flash")
            self.assertEqual(len(request["messages"]), 1)
            self.assertEqual(request["messages"][0]["role"], "user")
        self.assert_plain_requests(Provider.seen)
        self.assertEqual(self.rows("SELECT html,title,read,position FROM entries"), [(self.html, "English title", 0, 0)])
        self.assertEqual(self.rows("SELECT state FROM article_translations"), [("ready",)])
        self.assertEqual(self.rows("SELECT html FROM article_translation_blocks WHERE ordinal=0"), [("英文标题",)])
        cached = "".join(row[0] for row in self.rows("SELECT html FROM article_translation_blocks WHERE entry_id=1 ORDER BY ordinal"))
        self.assertIn("<strong>", cached)
        self.assertRegex(cached, r"Use RSSKEEP[0-9]+X0Q 读者\.")
        self.assertNotIn("keep_inline()", cached)
        self.assertIn("<pre>print('keep code')</pre>", cached)
        self.cli("translate", "1")
        self.assertEqual(len(Provider.seen), first)
        with sqlite3.connect(self.db) as conn:
            conn.execute("INSERT INTO settings(key,value) VALUES('reader_mode:rss:1','bilingual')")
        rendered = pathlib.Path(self.folder.name) / "bilingual.pgm"
        self.cli("--article", "1", "render", str(rendered))
        self.assertTrue(rendered.exists())

    def test_complex_links_translate_as_text_and_keep_original_markup(self):
        html = ('<p>Visit <a href="https://example.invalid/first">first source</a>, then '
                '<strong>read</strong> <a href="https://example.invalid/second">second source</a>.</p>')
        with sqlite3.connect(self.db) as conn:
            conn.execute("INSERT INTO entries(id,feed_id,guid,title,url,html,text) VALUES(2,1,'two','Complex links','https://example.invalid/complex',?,?)",
                         (html, "Visit first source, then read second source."))
        self.cli("translate", "2")
        self.assert_plain_requests(Provider.seen)
        self.assertCountEqual([request["messages"][0]["content"] for _, _, request in Provider.seen],
                               ["Complex links", "Visit", "first source", ", then", "read", "second source"])
        cached = "".join(row[0] for row in self.rows("SELECT html FROM article_translation_blocks WHERE entry_id=2 ORDER BY ordinal"))
        self.assertIn('<a href="https://example.invalid/first">第一链接</a>', cached)
        self.assertIn('<a href="https://example.invalid/second">第二链接</a>', cached)
        self.assertIn("<strong>阅读</strong>", cached)
        self.assertNotIn("< a href", cached)
        rendered = pathlib.Path(self.folder.name) / "complex.pgm"
        self.cli("--article", "2", "render", str(rendered))
        self.assertTrue(rendered.exists())

    def test_partial_failure_resumes_only_missing_blocks(self):
        self.endpoint = "/partial"
        self.cli("translate", "1", expected=2)
        self.assertEqual(self.rows("SELECT count(*) FROM article_translation_blocks"), [(1,)])
        self.assertEqual(self.rows("SELECT state FROM article_translations"), [("failed",)])
        cached = self.rows("SELECT html FROM article_translation_blocks WHERE ordinal=0")
        self.endpoint = "/ok"
        self.cli("translate", "1")
        self.assertEqual(self.rows("SELECT html FROM article_translation_blocks WHERE ordinal=0"), cached)
        for path, _, request in Provider.seen:
            if path == "/ok":
                self.assertNotIn("English title", request["messages"][0]["content"])
        self.assert_plain_requests([item for item in Provider.seen if item[0] == "/ok"])

    def test_rejects_provider_failures_without_retry_or_key_leak(self):
        for endpoint in ("/unauthorized", "/rate", "/redirect", "/truncated", "/hostile", "/empty", "/bad-json", "/large"):
            with self.subTest(endpoint=endpoint):
                self.endpoint = endpoint
                before = len(Provider.seen)
                self.cli("translate", "1", expected=2)
                self.assertEqual(len(Provider.seen), before + 1)
                self.assert_plain_requests(Provider.seen[before:])
                self.assertEqual(self.rows("SELECT count(*) FROM article_translation_blocks"), [(0,)])
        self.assertFalse(any(path == "/stolen" for path, _, _ in Provider.seen))

    def test_failure_after_first_text_node_keeps_block_atomic(self):
        self.endpoint = "/partial-node"
        self.cli("translate", "1", expected=2)
        self.assertEqual(len(Provider.seen), 3)
        self.assertEqual(self.rows("SELECT ordinal,html FROM article_translation_blocks"), [(0, "英文标题")])
        self.assertEqual(self.rows("SELECT html,read,position FROM entries"), [(self.html, 0, 0)])
        self.endpoint = "/ok"
        self.cli("translate", "1")
        self.assertEqual(self.rows("SELECT state FROM article_translations"), [("ready",)])
        resumed = [request["messages"][0]["content"] for path, _, request in Provider.seen if path == "/ok"]
        self.assertIn("A", resumed)
        self.assertNotIn("English title", resumed)

    def test_missing_key_sends_no_request(self):
        self.cli("translate", "1", expected=1, key=False)
        self.assertEqual(Provider.seen, [])

    def test_cancels_in_flight_request_and_keeps_previous_blocks(self):
        result = subprocess.run([sys.argv[2], self.base + "/slow"], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(any(path == "/slow" for path, _, _ in Provider.seen))
        self.assert_plain_requests(Provider.seen)

    def test_rss_and_web_cache_are_independent(self):
        self.cli("translate", "1")
        with sqlite3.connect(self.db) as conn:
            conn.execute("INSERT INTO article_fulltext(entry_id,html,text,url,state) VALUES(1,'<p>Offline reading matters.</p>','Offline reading matters.','https://example.invalid/story','ready')")
        self.cli("translate-web", "1")
        self.assertEqual(self.rows("SELECT web,state FROM article_translations ORDER BY web"), [(0, "ready"), (1, "ready")])
        self.assert_plain_requests(Provider.seen)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
