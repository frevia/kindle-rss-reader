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
        body=RSS.replace(b"<p>cached body</p>",b"<p>cached body</p><img src='/photo.png' alt='photo'><img src='/missing.png'>")
        with FeedHandler.lock:
            FeedHandler.responses["/photos"]=(200,{},body)
            FeedHandler.responses["/photo.png"]=(200,{"Content-Type":"image/png"},png)
        self.add("/photos");result=self.cli("sync")
        self.assertIn("1 张图片未缓存",result.stdout)
        self.assertEqual(self.rows("SELECT text FROM entries"),[("cached body",)])
        rows=self.rows("SELECT width,height,length(pixels),error FROM article_images ORDER BY ordinal")
        self.assertEqual(rows[0],(2,2,4,""));self.assertTrue(rows[1][3])
        requests=len(FeedHandler.seen_headers["/photo.png"])
        self.cli("sync")
        self.assertEqual(len(FeedHandler.seen_headers["/photo.png"]),requests)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
