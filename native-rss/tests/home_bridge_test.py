#!/usr/bin/env python3
import base64
import fcntl
import hashlib
import json
import os
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


BIN = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("HOME_BRIDGE_BIN")


def make_db(path, items=True):
    db = sqlite3.connect(path)
    db.executescript("""
        CREATE TABLE feeds(id INTEGER PRIMARY KEY, title TEXT NOT NULL DEFAULT '', active INTEGER NOT NULL DEFAULT 1, synced_at INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE entries(id INTEGER PRIMARY KEY, feed_id INTEGER NOT NULL, title TEXT NOT NULL DEFAULT '', read INTEGER NOT NULL DEFAULT 0, published INTEGER NOT NULL DEFAULT 0);
    """)
    db.executemany("INSERT INTO feeds VALUES(?,?,?,?)", [(1, 'F"eed\\', 1, 50), (2, 'off', 0, 99)])
    if items:
        db.executemany("INSERT INTO entries VALUES(?,?,?,?,?)", [
            (1, 1, 'read-new', 1, 99), (2, 1, 'unread-old', 0, 10),
            (3, 2, 'inactive "unread\\', 0, 1000),
        ])
    db.commit()
    return db


def read_frame(sock):
    def exact(n):
        data = bytearray()
        while len(data) < n:
            chunk = sock.recv(n-len(data))
            if not chunk:
                break
            data.extend(chunk)
        return bytes(data)
    first = exact(2)
    if len(first) != 2:
        raise AssertionError("incomplete WebSocket frame")
    n = first[1] & 127
    if n == 126:
        n = struct.unpack("!H", exact(2))[0]
    elif n == 127:
        n = struct.unpack("!Q", exact(8))[0]
    return first[0] & 15, exact(n)


def masked(opcode, data):
    mask = b"\x11\x22\x33\x44"
    return bytes([0x80 | opcode, 0x80 | len(data)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data))


class HomeBridgeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BIN:
            raise unittest.SkipTest("provide executable path as argv[1] or HOME_BRIDGE_BIN")
        cls.bin = str(Path(BIN).resolve())

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.db_path = self.root / "rss.sqlite"
        self.lock_path = self.root / "session.lock"
        self.state_path = self.root / "bridge.state"

    def run_summary(self):
        return subprocess.check_output([self.bin, "--db", str(self.db_path), "summary"], text=True)

    def server(self, db=None, extra=()):
        args = [self.bin, "--db", str(db or self.db_path), "--port", "0", "--session-lock", str(self.lock_path)]
        if self.state_path:
            args += ["--state", str(self.state_path)]
        args += list(extra) + ["serve"]
        p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.stop, p)
        ready = json.loads(p.stdout.readline())
        self.assertEqual(ready["event"], "listening")
        return p, ready["port"]

    def connect(self, port, origin=None, partial=False):
        s = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.addCleanup(s.close)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ("GET /rss-home/v1 HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
               "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n")
        if origin is not None:
            req += "Origin: " + origin + "\r\n"
        req += "\r\n"
        if partial:
            s.sendall(req[:10].encode())
            return s
        s.sendall(req.encode())
        data = bytearray()
        while b"\r\n\r\n" not in data:
            data.extend(s.recv(1))
        return s, bytes(data)

    def ws(self, port):
        s, handshake = self.connect(port)
        self.assertIn(b"101 Switching Protocols", handshake)
        opcode, payload = read_frame(s)
        self.assertEqual(opcode, 1)
        return s, json.loads(payload)

    def stop(self, p):
        if p.poll() is None:
            p.terminate()
            p.wait(timeout=3)
        if p.returncode != 0 and p.stderr:
            details = p.stderr.read()
            self.fail(f"server exited {p.returncode}: {details}")
        if p.stdout:
            p.stdout.close()
        if p.stderr:
            p.stderr.close()

    def test_summary_read_only_sorting_and_json_escaping(self):
        db = make_db(self.db_path)
        before = self.db_path.read_bytes()
        value = json.loads(self.run_summary())
        self.assertEqual((value["status"], value["unread"], value["feeds"], value["updatedAt"]), ("ready", 2, 1, 50))
        self.assertEqual([x["id"] for x in value["items"]], [3, 2])
        self.assertEqual(value["items"][0]["source"], "off")
        self.assertEqual(value["items"][0]["title"], 'inactive "unread\\')
        self.assertEqual(value["feedCards"], [{"id": 1, "title": 'F"eed\\', "unread": 1, "entryId": 2, "latestTitle": "unread-old"}])
        self.assertEqual(self.db_path.read_bytes(), before)
        self.assertEqual(json.loads(self.run_summary())["revision"], value["revision"])
        canonical = dict(value); canonical.pop("revision")
        expected = hashlib.sha256(json.dumps(canonical, ensure_ascii=False, separators=(",", ":")).encode()).hexdigest()
        self.assertEqual(value["revision"], expected)
        self.assertEqual(db.execute("select count(*) from entries").fetchone()[0], 3)

    def test_home_visible_defaults_and_changes_read_only_summary_revision(self):
        db = make_db(self.db_path)
        before = self.db_path.read_bytes()
        default = json.loads(self.run_summary())
        self.assertIs(default["enabled"], True)
        self.assertEqual(self.db_path.read_bytes(), before)

        db.execute("CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL)")
        db.commit(); db.close()
        no_key = json.loads(self.run_summary())
        self.assertIs(no_key["enabled"], True)
        self.assertEqual(default["revision"], no_key["revision"])

        db = sqlite3.connect(self.db_path)
        db.execute("INSERT INTO settings VALUES('home_visible','0')")
        db.commit(); db.close()
        disabled = json.loads(self.run_summary())
        self.assertIs(disabled["enabled"], False)
        self.assertNotEqual(no_key["revision"], disabled["revision"])

        db = sqlite3.connect(self.db_path)
        db.execute("UPDATE settings SET value='1' WHERE key='home_visible'")
        db.commit(); db.close()
        before = self.db_path.read_bytes()
        enabled = json.loads(self.run_summary())
        self.assertIs(enabled["enabled"], True)
        self.assertNotEqual(disabled["revision"], enabled["revision"])
        self.assertEqual(enabled["items"], disabled["items"])
        self.assertEqual(enabled["feedCards"], disabled["feedCards"])
        self.assertEqual(self.db_path.read_bytes(), before)

    def test_unavailable_missing_and_empty_valid_database(self):
        value = json.loads(self.run_summary())
        self.assertEqual(value["status"], "unavailable")
        self.assertEqual(value["feedCards"], [])
        self.assertFalse(self.db_path.exists())
        make_db(self.db_path, items=False).close()
        db = sqlite3.connect(self.db_path)
        db.execute("DELETE FROM feeds"); db.commit(); db.close()
        self.assertEqual(json.loads(self.run_summary())["status"], "empty")

    def test_unicode_codepoint_limit_invalid_utf8_and_controls(self):
        db = make_db(self.db_path)
        db.execute("UPDATE entries SET title=? WHERE id=3", ("界" * 47 + "\n\"\\",))
        db.commit(); db.close()
        item = json.loads(self.run_summary())["items"][0]
        self.assertEqual(len(item["title"]), 48)
        self.assertEqual(item["title"][-1], " ")
        db = sqlite3.connect(self.db_path)
        db.execute("UPDATE entries SET title=CAST(? AS TEXT) WHERE id=3", (sqlite3.Binary(b"bad\xff"),))
        db.commit(); db.close()
        item = json.loads(self.run_summary())["items"][0]
        self.assertEqual(item["title"], "bad\ufffd")

    def test_feed_cards_aggregate_active_feeds_and_sort_stably(self):
        db = make_db(self.db_path)
        db.executemany("INSERT INTO feeds VALUES(?,?,?,?)", [
            (3, "empty", 1, 0), (4, "tie-later-id", 1, 0), (5, "fourth", 1, 0),
            (6, "tie-first-id", 1, 0), (7, "inactive-new", 0, 0),
        ])
        db.executemany("INSERT INTO entries VALUES(?,?,?,?,?)", [
            (10, 4, "tie earlier id", 0, 200), (11, 4, "tie unread", 0, 200),
            (12, 6, "same time", 0, 200), (13, 5, "fourth", 0, 90),
            (14, 7, "inactive", 0, 300),
        ])
        db.commit(); db.close()
        before = self.db_path.read_bytes()
        cards = json.loads(self.run_summary())["feedCards"]
        self.assertEqual([card["id"] for card in cards], [4, 6, 1])
        self.assertEqual(cards[0], {"id": 4, "title": "tie-later-id", "unread": 2, "entryId": 11, "latestTitle": "tie unread"})
        self.assertEqual(cards[1]["latestTitle"], "same time")
        self.assertEqual(cards[1]["entryId"], 12)
        self.assertEqual(cards[2], {"id": 1, "title": 'F"eed\\', "unread": 1, "entryId": 2, "latestTitle": "unread-old"})
        self.assertEqual(len(cards), 3)
        self.assertEqual(self.db_path.read_bytes(), before)

        db = sqlite3.connect(self.db_path)
        db.execute("DELETE FROM entries WHERE feed_id IN (4,5,6,7)")
        db.execute("DELETE FROM feeds WHERE id IN (4,5,6,7)")
        db.commit(); db.close()
        cards = json.loads(self.run_summary())["feedCards"]
        self.assertEqual([card["id"] for card in cards], [1, 3])
        self.assertEqual(cards[1], {"id": 3, "title": "empty", "unread": 0, "entryId": 0, "latestTitle": ""})

    def test_feed_card_fields_change_revision_and_are_sanitized(self):
        db = make_db(self.db_path)
        db.execute("UPDATE feeds SET title=? WHERE id=1", ("界" * 30 + "\n\"\\",))
        db.execute("UPDATE entries SET title=CAST(? AS TEXT) WHERE id=2", (sqlite3.Binary(b"latest\xff"),))
        db.commit(); db.close()
        first = json.loads(self.run_summary())
        card = first["feedCards"][0]
        self.assertEqual(len(card["title"]), 24)
        self.assertEqual(card["title"], "界" * 24)
        self.assertEqual(card["latestTitle"], "latest\ufffd")
        db = sqlite3.connect(self.db_path)
        db.execute("UPDATE entries SET title='changed newest' WHERE id=2")
        db.commit(); db.close()
        second = json.loads(self.run_summary())
        self.assertNotEqual(first["revision"], second["revision"])
        self.assertNotEqual(second["items"], first["items"])
        self.assertEqual(second["feedCards"][0]["latestTitle"], "changed newest")
        _, port = self.server()
        peer, initial = self.ws(port)
        db = sqlite3.connect(self.db_path)
        db.execute("UPDATE feeds SET title='pushed source only' WHERE id=1")
        db.commit(); db.close()
        peer.settimeout(4)
        _, payload = read_frame(peer)
        pushed = json.loads(payload)
        self.assertEqual([item["id"] for item in pushed["items"]], [item["id"] for item in initial["items"]])
        self.assertEqual([item["title"] for item in pushed["items"]], [item["title"] for item in initial["items"]])
        self.assertEqual(pushed["items"][1]["source"], "pushed source only")
        self.assertNotEqual(pushed["revision"], initial["revision"])
        self.assertEqual(pushed["feedCards"][0]["title"], "pushed source only")
        self.assertEqual(pushed["feedCards"][0]["latestTitle"], "changed newest")
        peer.close()

    def test_article_links_are_atomic_stable_and_contain_only_numeric_target(self):
        make_db(self.db_path).close()
        folder = self.root / "open"
        _, port = self.server(extra=("--open-dir", str(folder)))
        peer, value = self.ws(port)
        self.assertEqual(value["feedCards"][0]["entryId"], 2)
        script = folder / "entry-2.sh"
        content = script.read_text()
        self.assertIn("exec sh /mnt/us/native-rss/launch.sh --article 2\n", content)
        self.assertNotIn('F"eed', content)
        self.assertEqual(script.stat().st_mode & 0o777, 0o755)
        before = script.stat()
        db = sqlite3.connect(self.db_path)
        db.execute("UPDATE entries SET read=1 WHERE id=2")
        db.commit(); db.close()
        peer.settimeout(4); _, payload = read_frame(peer)
        self.assertEqual(json.loads(payload)["feedCards"][0]["entryId"], 1)
        self.assertTrue((folder / "entry-1.sh").exists())
        self.assertEqual((script.stat().st_ino, script.stat().st_mtime_ns), (before.st_ino, before.st_mtime_ns))
        self.assertEqual(list(folder.glob(".entry-*")), [])
        peer.close()

    def test_article_link_directory_and_existing_files_do_not_follow_symlinks(self):
        make_db(self.db_path).close()
        real = self.root / "protected"
        real.mkdir()
        alias = self.root / "open"
        alias.symlink_to(real, target_is_directory=True)
        first_server, port = self.server(extra=("--open-dir", str(alias)))
        peer, value = self.ws(port)
        self.assertEqual(value["status"], "unavailable")
        self.assertEqual(list(real.iterdir()), [])
        peer.close(); self.stop(first_server)
        alias.unlink(); alias.mkdir()
        protected = real / "keep"
        protected.write_text("untouched")
        (alias / "entry-2.sh").symlink_to(protected)
        _, port = self.server(extra=("--open-dir", str(alias)))
        peer, value = self.ws(port)
        self.assertEqual(value["status"], "unavailable")
        self.assertEqual(protected.read_text(), "untouched")
        peer.close()

    def test_feed_card_only_change_pushes_when_global_preview_is_unchanged(self):
        db = make_db(self.db_path)
        db.execute("INSERT INTO feeds VALUES(3,'third',1,0)")
        db.execute("INSERT INTO entries VALUES(4,3,'old unread',0,1)")
        db.commit(); db.close()
        _, port = self.server()
        peer, original = self.ws(port)
        db = sqlite3.connect(self.db_path)
        db.execute("UPDATE entries SET title='changed card only' WHERE id=4")
        db.commit(); db.close()
        peer.settimeout(4); _, payload = read_frame(peer)
        pushed = json.loads(payload)
        self.assertEqual(pushed["items"], original["items"])
        self.assertNotEqual(pushed["revision"], original["revision"])
        self.assertEqual(next(card for card in pushed["feedCards"] if card["id"] == 3)["latestTitle"], "changed card only")
        peer.close()

    def test_server_push_ack_lock_and_sigterm(self):
        make_db(self.db_path).close()
        p, port = self.server()
        try:
            s, value = self.ws(port)
            self.assertEqual(value["items"][0]["id"], 3)
            ack = json.dumps({"type": "rss-home-ack", "revision": value["revision"]}, separators=(",", ":")).encode()
            s.sendall(masked(1, ack))
            deadline = time.time() + 2
            while time.time() < deadline and "ready=1" not in self.state_path.read_text():
                time.sleep(.05)
            self.assertIn("ready=1", self.state_path.read_text())
            db = sqlite3.connect(self.db_path)
            db.execute("INSERT INTO entries VALUES(4,1,'changed',0,1001)")
            db.commit(); db.close()
            s.settimeout(4)
            _, changed = read_frame(s)
            self.assertEqual(json.loads(changed)["items"][0]["id"], 4)
            with open(self.lock_path, "w") as lockfile:
                import fcntl
                fcntl.flock(lockfile, fcntl.LOCK_EX | fcntl.LOCK_NB)
                db = sqlite3.connect(self.db_path)
                db.execute("INSERT INTO entries VALUES(5,1,'locked',0,1002)")
                db.commit(); db.close()
                s.settimeout(2.4)
                with self.assertRaises(socket.timeout):
                    read_frame(s)
                fcntl.flock(lockfile, fcntl.LOCK_UN)
            s.settimeout(4)
            _, resumed = read_frame(s)
            self.assertEqual(json.loads(resumed)["items"][0]["id"], 5)
            s.close()
        finally:
            self.stop(p)
        self.assertIn("ready=0", self.state_path.read_text())

    def test_single_unread_does_not_include_read_articles(self):
        db = make_db(self.db_path)
        db.execute("UPDATE entries SET read=1 WHERE id=3")
        db.commit(); db.close()
        value = json.loads(self.run_summary())
        self.assertEqual(value["unread"], 1)
        self.assertEqual([item["id"] for item in value["items"]], [2])

    def test_marking_selected_unread_read_falls_back_and_pushes(self):
        make_db(self.db_path).close()
        _, port = self.server()
        peer, initial = self.ws(port)
        card = initial["feedCards"][0]
        self.assertEqual((card["unread"], card["entryId"], card["latestTitle"]), (1, 2, "unread-old"))
        db = sqlite3.connect(self.db_path)
        db.execute("UPDATE entries SET read=1 WHERE id=2")
        db.commit(); db.close()
        peer.settimeout(4)
        _, payload = read_frame(peer)
        updated = json.loads(payload)
        card = updated["feedCards"][0]
        self.assertEqual((card["unread"], card["entryId"], card["latestTitle"]), (0, 1, "read-new"))
        self.assertNotEqual(updated["revision"], initial["revision"])
        peer.close()

    def test_new_client_refresh_broadcasts_to_existing_client(self):
        make_db(self.db_path).close()
        _, port = self.server()
        first, original = self.ws(port)
        db = sqlite3.connect(self.db_path)
        db.execute("INSERT INTO entries VALUES(4,1,'new connection refresh',0,1001)")
        db.commit(); db.close()
        second, latest = self.ws(port)
        self.assertNotEqual(original["revision"], latest["revision"])
        self.assertEqual(latest["feedCards"][0]["latestTitle"], "new connection refresh")
        first.settimeout(4)
        _, payload = read_frame(first)
        pushed = json.loads(payload)
        self.assertEqual(pushed["revision"], latest["revision"])
        self.assertEqual(pushed["feedCards"][0]["latestTitle"], "new connection refresh")
        first.close(); second.close()

    def test_home_visible_updates_push_without_changing_reading_snapshot(self):
        db = make_db(self.db_path)
        db.execute("CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL)")
        db.execute("INSERT INTO settings VALUES('home_visible','1')")
        db.commit(); db.close()
        _, port = self.server()
        peer, initial = self.ws(port)
        self.assertIs(initial["enabled"], True)

        db = sqlite3.connect(self.db_path)
        db.execute("UPDATE settings SET value='0' WHERE key='home_visible'")
        db.commit(); db.close()
        peer.settimeout(5)
        _, payload = read_frame(peer)
        disabled = json.loads(payload)
        self.assertIs(disabled["enabled"], False)
        self.assertNotEqual(initial["revision"], disabled["revision"])
        self.assertEqual(disabled["items"], initial["items"])
        self.assertEqual(disabled["feedCards"], initial["feedCards"])
        self.assertEqual(disabled["unread"], initial["unread"])

        db = sqlite3.connect(self.db_path)
        db.execute("UPDATE settings SET value='1' WHERE key='home_visible'")
        db.commit(); db.close()
        _, payload = read_frame(peer)
        enabled = json.loads(payload)
        self.assertIs(enabled["enabled"], True)
        self.assertNotEqual(disabled["revision"], enabled["revision"])
        self.assertEqual(enabled["items"], initial["items"])
        self.assertEqual(enabled["feedCards"], initial["feedCards"])
        self.assertEqual(enabled["unread"], initial["unread"])
        peer.close()

    def test_initial_busy_database_recovers_without_another_write(self):
        make_db(self.db_path).close()
        _, port = self.server()
        writer = sqlite3.connect(self.db_path)
        writer.execute("BEGIN EXCLUSIVE")
        try:
            peer, unavailable = self.ws(port)
            self.assertEqual(unavailable["status"], "unavailable")
        finally:
            writer.rollback(); writer.close()
        peer.settimeout(4)
        _, payload = read_frame(peer)
        self.assertEqual(json.loads(payload)["status"], "ready")
        peer.close()

    def test_unchanged_health_is_not_rewritten(self):
        make_db(self.db_path).close()
        _, port = self.server()
        peer, value = self.ws(port)
        ack = json.dumps({"type": "rss-home-ack", "revision": value["revision"]}, separators=(",", ":")).encode()
        peer.sendall(masked(1, ack))
        deadline = time.time() + 3
        while time.time() < deadline and "ready=1" not in self.state_path.read_text():
            time.sleep(.05)
        self.assertIn("ready=1", self.state_path.read_text())
        before = self.state_path.stat()
        time.sleep(2.3)
        after = self.state_path.stat()
        self.assertEqual((before.st_ino, before.st_mtime_ns), (after.st_ino, after.st_mtime_ns))
        peer.close()

    def test_heartbeat_has_no_snapshot_ack_or_flash_write(self):
        make_db(self.db_path).close()
        _, port = self.server()
        peer, value = self.ws(port)
        ack = json.dumps({"type": "rss-home-ack", "revision": value["revision"]}, separators=(",", ":")).encode()
        peer.sendall(masked(1, ack))
        deadline = time.time() + 3
        while time.time() < deadline and "ready=1" not in self.state_path.read_text():
            time.sleep(.05)
        self.assertIn("ready=1", self.state_path.read_text())
        before = self.state_path.stat()
        peer.settimeout(18)
        opcode, payload = read_frame(peer)
        self.assertEqual(opcode, 1)
        self.assertEqual(json.loads(payload), {"type": "rss-home-heartbeat"})
        after = self.state_path.stat()
        self.assertEqual((before.st_ino, before.st_mtime_ns), (after.st_ino, after.st_mtime_ns))
        peer.close()

    def test_origin_rejected_and_masked_ping(self):
        make_db(self.db_path).close()
        p, port = self.server()
        try:
            s, response = self.connect(port, origin="http://evil")
            self.assertIn(b"403", response)
            s.close()
            s, _ = self.ws(port)
            s.sendall(masked(9, b"hi"))
            opcode, payload = read_frame(s)
            self.assertEqual((opcode, payload), (10, b"hi"))
            s.sendall(b"\x81\x02hi")
            opcode, _ = read_frame(s)
            self.assertEqual(opcode, 8)
            s.close()
        finally:
            self.stop(p)

    def test_session_lock_delays_initial_snapshot(self):
        make_db(self.db_path).close()
        p, port = self.server()
        lockfile = open(self.lock_path, "w")
        fcntl.flock(lockfile, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            s, response = self.connect(port)
            self.assertIn(b"101 Switching Protocols", response)
            s.settimeout(.4)
            with self.assertRaises(socket.timeout):
                read_frame(s)
            fcntl.flock(lockfile, fcntl.LOCK_UN)
            lockfile.close()
            s.settimeout(3)
            _, snapshot = read_frame(s)
            self.assertEqual(json.loads(snapshot)["status"], "ready")
            s.close()
        finally:
            if not lockfile.closed:
                fcntl.flock(lockfile, fcntl.LOCK_UN)
                lockfile.close()
            self.stop(p)

    def test_busy_database_keeps_last_snapshot(self):
        make_db(self.db_path).close()
        p, port = self.server()
        try:
            s, _ = self.ws(port)
            writer = sqlite3.connect(self.db_path, timeout=1)
            writer.execute("BEGIN EXCLUSIVE")
            writer.execute("UPDATE entries SET title='busy update' WHERE id=3")
            s.settimeout(2.4)
            with self.assertRaises(socket.timeout):
                read_frame(s)
            writer.commit(); writer.close()
            s.settimeout(4)
            _, fresh = read_frame(s)
            self.assertEqual(json.loads(fresh)["items"][0]["title"], "busy update")
            s.close()
        finally:
            self.stop(p)

    def test_slow_handshake_does_not_block_other_clients_or_exceed_limit(self):
        make_db(self.db_path).close()
        p, port = self.server()
        try:
            slow = self.connect(port, partial=True)
            peers = [self.ws(port)[0] for _ in range(3)]
            refused = socket.create_connection(("127.0.0.1", port), timeout=2)
            self.addCleanup(refused.close)
            refused.settimeout(1)
            self.assertEqual(refused.recv(1), b"")
            for peer in peers:
                peer.close()
            slow.close()
        finally:
            self.stop(p)

    def test_atomic_replacement_and_missing_database_recovery(self):
        make_db(self.db_path).close()
        p, port = self.server()
        try:
            s, value = self.ws(port)
            replacement = self.root / "replacement.sqlite"
            db = make_db(replacement)
            db.execute("INSERT INTO entries VALUES(8,1,'replacement',0,1000)"); db.commit(); db.close()
            os.replace(replacement, self.db_path)
            s.settimeout(4)
            _, fresh = read_frame(s)
            self.assertEqual(json.loads(fresh)["items"][0]["id"], 8)
            self.db_path.unlink()
            time.sleep(2.2)
            make_db(self.db_path).close()
            s.settimeout(4)
            _, restored = read_frame(s)
            self.assertEqual(json.loads(restored)["status"], "ready")
            s.close()
        finally:
            self.stop(p)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
