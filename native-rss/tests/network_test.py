#!/usr/bin/env python3
"""Local-only end-to-end checks for the native RSS CLI and HTTP client."""

import http.server
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest
from urllib.parse import urlsplit


RSS = b'''<?xml version="1.0"?><rss version="2.0"><channel><title>Local News</title>
<item><guid>item-1</guid><title>First story</title><link>/story</link>
<description><![CDATA[<p>cached body</p>]]></description><pubDate>Tue, 02 Jan 2024 12:00:00 GMT</pubDate></item>
</channel></rss>'''
RSS_UPDATED = RSS.replace(b"First story", b"Updated story").replace(b"cached body", b"updated body")
ARTICLE = b'''<!doctype html><html><head><meta charset="utf-8"><title>Article</title></head>
<body><nav>Unwanted navigation</nav><article><h1>Full article</h1>
<p>This is the complete article with enough original prose to distinguish the story from its surrounding navigation.</p>
<blockquote>A quotation worth keeping.</blockquote><pre>code &lt; sample</pre>
<p>The second paragraph explains the subject in more detail and should remain available after the network is disconnected.</p>
<img src="../photo.png" alt="offline image"></article><footer>Unwanted footer</footer></body></html>'''


class FeedHandler(http.server.BaseHTTPRequestHandler):
    responses = {}
    seen_headers = {}
    lock = threading.Lock()

    def log_message(self, *_args):
        pass

    def do_GET(self):
        path = urlsplit(self.path).path
        with self.lock:
            self.seen_headers.setdefault(path, []).append(dict(self.headers.items()))
            response = self.responses.get(path, (404, {}, b"missing"))
        status, headers, body = response
        if path == "/conditional" and self.headers.get("If-None-Match") == '"v1"':
            status, headers, body = 304, {}, b""
        self.send_response(status)
        for key, value in headers.items():
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass


class NativeRSSNetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        FeedHandler.responses = {
            "/feed": (200, {"Content-Type": "application/rss+xml"}, RSS),
            "/conditional": (200, {"ETag": '"v1"'}, RSS),
            "/changing": (200, {}, RSS),
            "/bad": (200, {"Content-Type": "application/xml"}, b"<rss><channel><title>broken"),
            "/error": (500, {}, b"server error"),
            "/redirect-http": (302, {"Location": "/feed"}, b""),
            "/redirect-file": (302, {"Location": "file:///etc/passwd"}, b""),
            "/large": (200, {}, b"x" * (8 * 1024 * 1024 + 1)),
        }
        FeedHandler.seen_headers = {}
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FeedHandler)
        cls.server.daemon_threads = True
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.db = str(pathlib.Path(self.temp.name) / "rss.sqlite")
        binary = pathlib.Path(sys.argv[1]).resolve()
        self.binary = str(binary)

    def tearDown(self):
        self.temp.cleanup()

    def cli(self, *args, expected=(0,)):
        result = subprocess.run(
            [self.binary, "--db", self.db, *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=15,
        )
        self.assertIn(result.returncode, expected, result.stdout + result.stderr)
        return result

    def add(self, path):
        url = self.base + path
        self.cli("add", url)
        return url

    def rows(self, sql, params=()):
        with sqlite3.connect(self.db) as conn:
            return conn.execute(sql, params).fetchall()

    def test_feed_title_etag_304_and_updated_item_state(self):
        url = self.add("/conditional")
        self.cli("sync")
        feed = self.rows("SELECT title, etag, error FROM feeds WHERE url=?", (url,))[0]
        self.assertEqual(feed, ("Local News", '"v1"', ""))
        self.assertEqual(self.rows("SELECT title, text FROM entries"), [("First story", "cached body")])

        with sqlite3.connect(self.db) as conn:
            conn.execute("UPDATE entries SET read=1, starred=1, position=23")
        self.cli("sync")
        headers = FeedHandler.seen_headers["/conditional"]
        self.assertEqual(headers[-1].get("If-None-Match"), '"v1"')
        self.assertEqual(self.rows("SELECT title, read, starred, position FROM entries"), [("First story", 1, 1, 23)])
        self.assertEqual(self.rows("SELECT etag FROM feeds WHERE url=?", (url,)), [('"v1"',)])

        with FeedHandler.lock:
            FeedHandler.responses["/changing"] = (200, {}, RSS)
        changed_url = self.add("/changing")
        self.cli("sync")
        with sqlite3.connect(self.db) as conn:
            conn.execute("UPDATE entries SET read=1, starred=1, position=37 WHERE feed_id=(SELECT id FROM feeds WHERE url=?)", (changed_url,))
        with FeedHandler.lock:
            FeedHandler.responses["/changing"] = (200, {}, RSS_UPDATED)
        self.cli("sync")
        self.assertEqual(
            self.rows("SELECT title, text, read, starred, position FROM entries WHERE feed_id=(SELECT id FROM feeds WHERE url=?)", (changed_url,)),
            [("Updated story", "updated body", 1, 1, 37)],
        )

    def test_parse_failure_is_recorded_without_losing_cached_content(self):
        url = self.add("/feed")
        self.cli("sync")
        before = self.rows("SELECT guid, title, text FROM entries")
        with FeedHandler.lock:
            FeedHandler.responses["/feed"] = (200, {}, b"<html><body>Not a feed</body></html>")
        try:
            self.cli("sync", expected=(2,))
        finally:
            with FeedHandler.lock:
                FeedHandler.responses["/feed"] = (200, {}, RSS)
        self.assertEqual(self.rows("SELECT guid, title, text FROM entries WHERE feed_id=(SELECT id FROM feeds WHERE url=?)", (url,)), before)
        error = self.rows("SELECT error FROM feeds WHERE url=?", (url,))[0][0]
        self.assertTrue(error)

    def test_http_failure_does_not_block_other_feeds(self):
        bad_url = self.add("/error")
        good_url = self.add("/feed")
        result = self.cli("sync", expected=(2,))
        self.assertIn("1 个订阅失败", result.stdout)
        self.assertTrue(self.rows("SELECT error FROM feeds WHERE url=?", (bad_url,))[0][0])
        self.assertEqual(self.rows("SELECT title FROM entries"), [("First story",)])
        self.assertEqual(self.rows("SELECT error FROM feeds WHERE url=?", (good_url,))[0][0], "")

    def test_redirect_policy_http_allowed_file_rejected_and_size_bounded(self):
        good_url = self.add("/redirect-http")
        self.cli("sync")
        self.assertEqual(self.rows("SELECT title FROM feeds WHERE url=?", (good_url,))[0][0], "Local News")

        file_url = self.add("/redirect-file")
        large_url = self.add("/large")
        self.cli("sync", expected=(2,))
        failures = self.rows("SELECT url, error FROM feeds WHERE url IN (?, ?) ORDER BY url", (file_url, large_url))
        self.assertEqual(len(failures), 2)
        self.assertTrue(all(error for _, error in failures))
        self.assertEqual(self.rows("SELECT COUNT(*) FROM entries WHERE feed_id IN (SELECT id FROM feeds WHERE url IN (?, ?))", (file_url, large_url)), [(0,)])

    def test_exported_opml_imports_into_a_separate_database(self):
        first = self.add("/feed")
        second = self.add("/conditional")
        export_path = str(pathlib.Path(self.temp.name) / "subscriptions.opml")
        self.cli("export", export_path)
        target_db = str(pathlib.Path(self.temp.name) / "imported.sqlite")
        result = subprocess.run([self.binary, "--db", target_db, "import", export_path], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with sqlite3.connect(target_db) as conn:
            imported = {row[0] for row in conn.execute("SELECT url FROM feeds")}
        self.assertEqual(imported, {first, second})

    def test_images_are_cached_offline_and_failure_does_not_remove_text(self):
        png=(pathlib.Path(__file__).parent/"fixtures/colors.png").read_bytes()
        body=RSS.replace(b"<p>cached body</p>",b"<p>cached body</p><img src='/photo.png' alt='photo'><img src='/large.webp'><img src='/large.avif'><img src='/missing.png'>")
        with FeedHandler.lock:
            FeedHandler.responses["/photos"]=(200,{},body)
            FeedHandler.responses["/photo.png"]=(200,{"Content-Type":"image/png"},png)
            for path, fixture, mime in [("/large.webp","large-colors.webp","image/webp"),("/large.avif","large-10bit.avif","image/avif")]:
                FeedHandler.responses[path]=(200,{"Content-Type":mime},(pathlib.Path(__file__).parent/"fixtures"/fixture).read_bytes())
        self.add("/photos");result=self.cli("sync")
        self.assertIn("1 张图片未缓存",result.stdout)
        self.assertEqual(self.rows("SELECT text FROM entries"),[("cached body",)])
        rows=self.rows("SELECT width,height,length(pixels),error FROM article_images ORDER BY ordinal")
        self.assertEqual(rows[0],(2,2,4,""));self.assertEqual(rows[1],(944,3,2832,""));self.assertEqual(rows[2],(944,20,18880,""));self.assertTrue(rows[3][3])
        requests={path:len(FeedHandler.seen_headers[path]) for path in ["/photo.png","/large.webp","/large.avif"]}
        self.cli("sync")
        self.assertEqual({path:len(FeedHandler.seen_headers[path]) for path in requests},requests)

    def article_feed(self, path, article_path="/article-redirect", auto=False):
        body = RSS.replace(b"/story", article_path.encode())
        with FeedHandler.lock:
            FeedHandler.responses[path] = (200, {"ETag": '"v1"'}, body)
            FeedHandler.responses["/article-redirect"] = (302, {"Location": "/articles/full"}, b"")
            FeedHandler.responses["/articles/full"] = (200, {"Content-Type": "text/html; charset=UTF-8"}, ARTICLE)
            FeedHandler.responses["/photo.png"] = (200, {"Content-Type": "image/png"}, (pathlib.Path(__file__).parent / "fixtures/colors.png").read_bytes())
        url = self.add(path)
        if auto:
            with sqlite3.connect(self.db) as conn:
                feed_id = conn.execute("SELECT id FROM feeds WHERE url=?", (url,)).fetchone()[0]
                conn.execute("INSERT INTO settings VALUES(?, '1')", (f"feed_fulltext:{feed_id}",))
        return url

    def test_manual_fulltext_preserves_original_state_and_images_across_rss_updates(self):
        url = self.article_feed("/manual-fulltext")
        self.cli("sync")
        entry_id = self.rows("SELECT id FROM entries")[0][0]
        with sqlite3.connect(self.db) as conn:
            conn.execute("UPDATE entries SET read=1,starred=1,position=37,in_progress=1")
        original = self.rows("SELECT html,text,read,starred,position,in_progress FROM entries")
        self.cli("fulltext", str(entry_id))
        self.assertEqual(self.rows("SELECT html,text,read,starred,position,in_progress FROM entries"), original)
        state, text, effective = self.rows("SELECT state,text,url FROM article_fulltext")[0]
        self.assertEqual(state, "ready")
        self.assertIn("complete article", text)
        self.assertNotIn("Unwanted", text)
        self.assertEqual(effective, self.base + "/articles/full")
        self.assertEqual(self.rows("SELECT width,height,length(pixels) FROM article_images"), [(2, 2, 4)])
        requests = len(FeedHandler.seen_headers["/articles/full"])
        self.cli("sync")
        self.cli("fulltext", str(entry_id))
        self.assertEqual(len(FeedHandler.seen_headers["/articles/full"]), requests)
        self.assertEqual(self.rows("SELECT html,text,read,starred,position,in_progress FROM entries"), original)
        self.assertEqual(self.rows("SELECT length(pixels) FROM article_images"), [(4,)])
        self.assertEqual(self.rows("PRAGMA foreign_key_check"), [])

    def test_auto_fulltext_only_new_articles_and_pending_work_survives_304(self):
        url = self.article_feed("/existing-fulltext")
        self.cli("sync")
        feed_id = self.rows("SELECT id FROM feeds WHERE url=?", (url,))[0][0]
        with sqlite3.connect(self.db) as conn:
            conn.execute("INSERT INTO settings VALUES(?, '1')", (f"feed_fulltext:{feed_id}",))
        self.cli("sync")
        self.assertEqual(self.rows("SELECT COUNT(*) FROM article_fulltext"), [(0,)])

        new_url = self.article_feed("/conditional", auto=True)
        with FeedHandler.lock:
            FeedHandler.responses["/articles/full"] = (500, {}, b"temporary failure")
        result = self.cli("sync", expected=(2,))
        self.assertIn("网页正文失败", result.stdout)
        self.assertEqual(self.rows("SELECT state FROM article_fulltext"), [("failed",)])
        self.assertEqual(self.rows("SELECT error FROM feeds WHERE url=?", (new_url,)), [("",)])
        with sqlite3.connect(self.db) as conn:
            conn.execute("UPDATE article_fulltext SET state='pending'")
        with FeedHandler.lock:
            FeedHandler.responses["/articles/full"] = (200, {"Content-Type": "text/html"}, ARTICLE)
        self.cli("sync")
        self.assertEqual(self.rows("SELECT state FROM article_fulltext"), [("ready",)])
        self.assertEqual(FeedHandler.seen_headers["/conditional"][-1].get("If-None-Match"), '"v1"')

    def test_fulltext_failures_keep_rss_and_manual_retry_recovers(self):
        cases = [
            ("mime", {"Content-Type": "application/pdf"}, b"%PDF document"),
            ("encoding", {"Content-Type": "text/html; charset=GBK"}, ARTICLE),
            ("login", {"Content-Type": "text/html"}, b"<html><body><h1>Sign in</h1><form><input type='password'></form></body></html>"),
            ("oversized", {"Content-Type": "text/html"}, b"x" * (8 * 1024 * 1024 + 1)),
        ]
        for name, headers, body in cases:
            with self.subTest(name=name):
                self.article_feed("/failure-" + name, "/failure-page-" + name)
                with FeedHandler.lock:
                    FeedHandler.responses["/failure-page-" + name] = (200, headers, body)
                self.cli("sync")
                entry_id = self.rows("SELECT id FROM entries WHERE url=?", (self.base + "/failure-page-" + name,))[0][0]
                self.cli("fulltext", str(entry_id), expected=(2,))
                self.assertEqual(self.rows("SELECT text FROM entries WHERE id=?", (entry_id,)), [("cached body",)])
                self.assertEqual(self.rows("SELECT state FROM article_fulltext WHERE entry_id=?", (entry_id,)), [("failed",)])
                with FeedHandler.lock:
                    FeedHandler.responses["/failure-page-" + name] = (200, {"Content-Type": "text/html"}, ARTICLE)
                self.cli("sync", "failed")
                self.assertEqual(self.rows("SELECT state FROM article_fulltext WHERE entry_id=?", (entry_id,)), [("ready",)])

    def test_auto_fulltext_batch_limit_leaves_durable_work_for_next_sync(self):
        url = self.article_feed("/batch-fulltext", auto=True)
        items = b"".join(
            b"<item><guid>batch-" + str(i).encode() + b"</guid><title>Batch story</title><link>/articles/full</link><description>RSS summary</description></item>"
            for i in range(12)
        )
        with FeedHandler.lock:
            FeedHandler.responses["/batch-fulltext"] = (200, {}, b"<rss><channel><title>Batch</title>" + items + b"</channel></rss>")
        first = self.cli("sync")
        self.assertIn("待下载", first.stdout)
        self.assertEqual(dict(self.rows("SELECT state,COUNT(*) FROM article_fulltext GROUP BY state")), {"pending": 2, "ready": 10})
        self.cli("sync")
        self.assertEqual(self.rows("SELECT state,COUNT(*) FROM article_fulltext GROUP BY state"), [("ready", 12)])

    def test_derived_requests_block_other_private_origins_and_unsafe_redirects(self):
        self.article_feed("/private-fulltext", "/private-redirect")
        with FeedHandler.lock:
            FeedHandler.responses["/private-redirect"] = (302, {"Location": self.base.replace("127.0.0.1", "localhost") + "/never-fetch"}, b"")
            FeedHandler.responses["/never-fetch"] = (200, {"Content-Type": "text/html"}, ARTICLE)
        self.cli("sync")
        entry_id = self.rows("SELECT id FROM entries")[0][0]
        self.cli("fulltext", str(entry_id), expected=(2,))
        self.assertNotIn("/never-fetch", FeedHandler.seen_headers)
        self.assertIn("内网", self.rows("SELECT error FROM article_fulltext")[0][0])
        with FeedHandler.lock:
            FeedHandler.responses["/private-redirect"] = (302, {"Location": "file:///etc/passwd"}, b"")
        self.cli("fulltext", str(entry_id), expected=(2,))
        self.assertEqual(self.rows("SELECT text FROM entries"), [("cached body",)])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
