#!/usr/bin/env python3
"""Exercise translation HTTP contracts against a local fake provider, with no real key."""
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
        if "translation_options" not in request:
            text = text.split("\n\n", 1)[1]
        # Old HTML-fragment requests trigger the malformed-link and dropped-link failure.
        translated = "< a href=\"https://example.invalid/lost\">漏链接</a>" if re.search(r"<\s*/?\w", text) else text
        for source, target in (("English title", "英文标题"), ("A quiet reader.", "A 安静读者。"),
                               ("Offline reading matters.", "离线阅读很重要。"),
                               ("First paragraph.", "第一段。"), ("Second paragraph.", "第二段。"),
                               ("Third paragraph.", "第三段。"),
                               ("Use", "使用"), ("reader.", "读者。"),
                               ("Visit first source, then read second source.", "访问第一链接，然后阅读第二链接。"),
                               ("Complex links", "复杂链接")):
            translated = translated.replace(source, target)
        response = {"choices": [{"message": {"role": "assistant", "content": translated}, "finish_reason": "stop"}]}
        headers = {}
        if self.path == "/slow":
            time.sleep(1.5)
        if self.path in ("/unauthorized", "/rate", "/error") or (self.path == "/paragraph-failure" and count == 3):
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
        self.provider = "qwen"
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
        env = {k: v for k, v in env.items() if not k.startswith(("RSS_TRANSLATION_", "RSS_QWEN_"))}
        env["RSS_TRANSLATION_API_KEY"] = "test-key" if key else ""
        env["RSS_TRANSLATION_ENDPOINT"] = self.base + self.endpoint
        if self.provider != "qwen":
            env["RSS_TRANSLATION_PROVIDER"] = self.provider
            if self.provider == "openai-compatible":
                env["RSS_TRANSLATION_MODEL"] = "custom-chat"
        result = subprocess.run([self.binary, "--db", str(self.db), *args], env=env,
                                text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertNotIn("test-key", result.stdout + result.stderr)
        return result

    def rows(self, sql, params=()):
        with sqlite3.connect(self.db) as conn:
            return conn.execute(sql, params).fetchall()

    def assert_plain_requests(self, requests):
        for _, _, request in requests:
            options = request["translation_options"]
            self.assertEqual({key: options[key] for key in ("source_lang", "target_lang")},
                             {"source_lang": "English", "target_lang": "Chinese"})
            self.assertEqual(len(request["messages"]), 1)
            content = request["messages"][0]["content"]
            self.assertNotIn("HTML fragment:", content)
            self.assertNotIn("English to Simplified Chinese", content)
            self.assertNotRegex(content, r"<\s*/?\w")
            self.assertNotRegex(content, r"https?://|mailto:")
            self.assertNotIn("href=", content)
            self.assertNotIn("<code", content)
            self.assertNotIn("<pre", content)
            self.assertNotIn("print('keep code')", content)
            self.assertNotIn("keep_inline()", content)
            tokens = re.findall(r"RSSKEEP\d+X\d+Q", content)
            terms = options.get("terms", [])
            self.assertCountEqual([term["source"] for term in terms], tokens)
            self.assertTrue(all(term["source"] == term["target"] for term in terms))
            self.assertEqual(len(tokens), len(set(tokens)))

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
        translated_cache = cached.replace("<pre>print('keep code')</pre>", "")
        self.assertNotRegex(translated_cache, r"<\s*/?\w")
        self.assertIn("<pre>print('keep code')</pre>", cached)
        self.assertIn("A 安静读者。", cached)
        self.assertRegex(cached, r"使用 RSSKEEP\d+X\d+Q 读者。")
        self.assertNotIn("keep_inline()", cached)
        self.assertNotIn("print('keep code')", translated_cache)
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
                              ["Complex links", "Visit first source, then read second source."])
        cached = "".join(row[0] for row in self.rows("SELECT html FROM article_translation_blocks WHERE entry_id=2 ORDER BY ordinal"))
        self.assertIn("访问第一链接，然后阅读第二链接。", cached)
        self.assertNotRegex(cached, r"<\s*/?\w")
        self.assertEqual(self.rows("SELECT html FROM entries WHERE id=2"), [(html,)])
        rendered = pathlib.Path(self.folder.name) / "complex.pgm"
        self.cli("--article", "2", "render", str(rendered))
        self.assertTrue(rendered.exists())

    def test_chat_providers_preserve_placeholders_and_reuse_cache(self):
        for provider, model in (("siliconflow", "tencent/Hunyuan-MT-7B"),
                                ("zhipu", "glm-4-flash-250414"), ("openai-compatible", "custom-chat")):
            with self.subTest(provider=provider):
                self.provider = provider
                with sqlite3.connect(self.db) as conn:
                    conn.execute("DELETE FROM article_translation_blocks")
                    conn.execute("DELETE FROM article_translations")
                with Provider.lock:
                    Provider.seen = []
                self.cli("translate", "1")
                first = len(Provider.seen)
                self.assertGreaterEqual(first, 3)
                for path, headers, request in Provider.seen:
                    self.assertEqual(path, "/ok")
                    self.assertEqual(headers["Authorization"], "Bearer test-key")
                    self.assertEqual(request["model"], model)
                    self.assertNotIn("translation_options", request)
                    self.assertFalse(request["stream"])
                    self.assertEqual([m["role"] for m in request["messages"]], ["user"])
                    prompt, text = request["messages"][0]["content"].split("\n\n", 1)
                    self.assertIn("Simplified Chinese", prompt)
                    self.assertNotRegex(text, r"<\s*/?\w|keep_inline|print\(")
                    for token in re.findall(r"RSSKEEP\d+X\d+Q", text):
                        self.assertIn(token, prompt)
                self.assertEqual(self.rows("SELECT state FROM article_translations"), [("ready",)])
                self.assertEqual(self.rows("SELECT html FROM entries"), [(self.html,)])
                self.assertEqual(self.rows("SELECT html FROM article_translation_blocks WHERE ordinal=0"), [("英文标题",)])
                self.cli("translate", "1")
                self.assertEqual(len(Provider.seen), first)

    def test_chat_provider_failures_do_not_retry_or_cache(self):
        self.provider = "siliconflow"
        for endpoint in ("/unauthorized", "/rate", "/redirect", "/truncated", "/hostile", "/bad-json"):
            with self.subTest(endpoint=endpoint):
                self.endpoint = endpoint
                before = len(Provider.seen)
                self.cli("translate", "1", expected=2)
                self.assertEqual(len(Provider.seen), before + 1)
                self.assertEqual(self.rows("SELECT count(*) FROM article_translation_blocks"), [(0,)])
        self.assertFalse(any(path == "/stolen" for path, _, _ in Provider.seen))

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

    def test_failure_after_second_paragraph_keeps_completed_blocks_and_resumes(self):
        html = "<p>First paragraph.</p><p>Second paragraph.</p><p>Third paragraph.</p>"
        with sqlite3.connect(self.db) as conn:
            conn.execute("INSERT INTO entries(id,feed_id,guid,title,url,html,text) VALUES(3,1,'three','Partial paragraphs','https://example.invalid/partial',?,?)",
                         (html, "First paragraph.\nSecond paragraph.\nThird paragraph."))
        self.endpoint = "/paragraph-failure"
        self.cli("translate", "3", expected=2)
        self.assertEqual(len(Provider.seen), 3)
        self.assertEqual(self.rows("SELECT ordinal,html FROM article_translation_blocks WHERE entry_id=3 ORDER BY ordinal"),
                         [(0, "Partial paragraphs"), (1, "第一段。")])
        self.assertEqual(self.rows("SELECT html,read,position FROM entries WHERE id=3"), [(html, 0, 0)])
        self.endpoint = "/ok"
        self.cli("translate", "3")
        self.assertEqual(self.rows("SELECT state FROM article_translations WHERE entry_id=3"), [("ready",)])
        resumed = [request["messages"][0]["content"] for path, _, request in Provider.seen if path == "/ok"]
        self.assertEqual(resumed, ["Second paragraph.", "Third paragraph."])
        self.assert_plain_requests([item for item in Provider.seen if item[0] == "/ok"])

    def test_v2_cache_survives_failure_until_v3_is_complete(self):
        old_revision = "rss-translation-v2:legacy"
        with sqlite3.connect(self.db) as conn:
            conn.execute("INSERT INTO article_translations(entry_id,web,revision,state,total) VALUES(1,0,?,'ready',2)", (old_revision,))
            conn.executemany("INSERT INTO article_translation_blocks(entry_id,web,revision,ordinal,html) VALUES(1,0,?,?,?)",
                             ((old_revision, 0, "legacy title"), (old_revision, 1, "legacy paragraph")))
        self.endpoint = "/error"
        self.cli("translate", "1", expected=2)
        self.assertEqual(self.rows("SELECT ordinal,html FROM article_translation_blocks WHERE entry_id=1 AND revision=? ORDER BY ordinal", (old_revision,)),
                         [(0, "legacy title"), (1, "legacy paragraph")])
        self.endpoint = "/ok"
        self.cli("translate", "1")
        self.assert_plain_requests(Provider.seen)
        self.assertTrue(any(request["messages"][0]["content"] == "English title" for _, _, request in Provider.seen))
        self.assertTrue(any(request["messages"][0]["content"] == "A quiet reader." for _, _, request in Provider.seen))
        revisions = self.rows("SELECT revision FROM article_translations WHERE entry_id=1 ORDER BY revision")
        self.assertNotIn((old_revision,), revisions)
        self.assertTrue(any(revision.startswith("rss-translation-v3:") for (revision,) in revisions))
        self.assertEqual(self.rows("SELECT count(*) FROM article_translation_blocks WHERE entry_id=1 AND revision=?", (old_revision,)), [(0,)])

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
