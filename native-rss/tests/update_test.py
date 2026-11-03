import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import warnings
import zipfile


VERSION = "v999.0.0"
REQUIRED = {
    "native-rss/bin/kindle-rss": b"new reader executable",
    "native-rss/launch.sh": b"#!/bin/sh\nexit 0\n",
    "native-rss/fonts/reader.ttf": b"font bytes",
    "native-rss/certs/ca-bundle.pem": b"certificate bytes",
    "extensions/native-rss/config.xml": b"<extension/>",
}


def payload(metadata=None):
    files = dict(REQUIRED)
    files["native-rss/UPDATE.json"] = json.dumps(
        metadata or {"format": 1, "model": "KPW3", "version": VERSION},
        separators=(",", ":"),
    ).encode()
    files["native-rss/docs/release-notes.md"] = b"release notes"
    manifest = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    files["native-rss/MANIFEST.json"] = json.dumps(
        {"files": manifest}, separators=(",", ":")
    ).encode()
    return files


def make_zip(path, files, extra=(), duplicate=None):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            info = zipfile.ZipInfo(name)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (0o100644 << 16)
            info.extra = b""
            archive.writestr(info, data)
        for name, data in extra:
            info = zipfile.ZipInfo(name)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (0o100644 << 16)
            info.extra = b""
            archive.writestr(info, data)
        if duplicate:
            name, data = duplicate
            info = zipfile.ZipInfo(name)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.extra = b""
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                archive.writestr(info, data)


class UpdateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.driver = DRIVER

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "root"
        (self.root / "native-rss/bin").mkdir(parents=True)
        (self.root / "native-rss/data").mkdir(parents=True)
        (self.root / "native-rss/bin/kindle-rss").write_bytes(b"old reader")
        (self.root / "native-rss/private.txt").write_bytes(b"user data")
        with sqlite3.connect(self.root / "native-rss/data/rss.sqlite") as db:
            db.execute("CREATE TABLE saved (value TEXT)")
            db.execute("INSERT INTO saved VALUES ('before update')")
        self.archive = self.base / "update.zip"
        self.files = payload()
        self.write_zip()

    def write_zip(self, files=None, **kwargs):
        make_zip(self.archive, self.files if files is None else files, **kwargs)

    def run_driver(self, *args, ok=True):
        result = subprocess.run(
            [self.driver, *map(str, args)], text=True, capture_output=True, check=False
        )
        if ok and result.returncode:
            self.fail(f"driver failed ({result.returncode}): {result.stderr}")
        if not ok and not result.returncode:
            self.fail("driver unexpectedly succeeded")
        return result

    def stage(self, *, archive=None, version=VERSION, digest=None, ok=True, cancel=False):
        archive = self.archive if archive is None else Path(archive)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest() if digest is None else digest
        args = ["stage", self.root, archive, version, digest]
        if cancel:
            args.append("cancel")
        return self.run_driver(*args, ok=ok)

    def ready_dir(self):
        return self.root / "native-rss/.update"

    def test_stage_validates_and_does_not_touch_installed_program_or_database(self):
        old = (self.root / "native-rss/bin/kindle-rss").read_bytes()
        database = (self.root / "native-rss/data/rss.sqlite").read_bytes()
        self.stage()
        self.assertEqual((self.root / "native-rss/bin/kindle-rss").read_bytes(), old)
        self.assertEqual((self.root / "native-rss/data/rss.sqlite").read_bytes(), database)
        self.assertEqual((self.ready_dir() / "ready").read_text(), VERSION)

    def test_apply_installs_update_and_preserves_private_files(self):
        self.stage()
        self.run_driver("apply", self.root)
        self.assertEqual((self.root / "native-rss/bin/kindle-rss").read_bytes(), REQUIRED["native-rss/bin/kindle-rss"])
        self.assertEqual((self.root / "native-rss/private.txt").read_bytes(), b"user data")
        self.assertEqual((self.root / "native-rss/docs/release-notes.md").read_bytes(), b"release notes")
        self.assertEqual((self.root / "native-rss/bin/kindle-rss").stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.root / "native-rss/launch.sh").stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.root / "native-rss/docs/release-notes.md").stat().st_mode & 0o777, 0o644)

    def test_streams_files_across_multiple_inflate_and_io_buffers(self):
        for data in (b"a" * (1024 * 1024), os.urandom(256 * 1024 + 17)):
            files = payload()
            files["native-rss/fonts/reader.ttf"] = data
            files["native-rss/MANIFEST.json"] = json.dumps({"files": {
                name: hashlib.sha256(value).hexdigest() for name, value in files.items()
                if name != "native-rss/MANIFEST.json"}}).encode()
            self.write_zip(files)
            self.stage()
            self.assertEqual((self.ready_dir() / "stage/native-rss/fonts/reader.ttf").read_bytes(), data)

    def test_recover_restores_old_files_database_and_removes_new_files_idempotently(self):
        self.stage()
        self.run_driver("backup", self.root)
        self.run_driver("apply", self.root)
        with sqlite3.connect(self.root / "native-rss/data/rss.sqlite") as db:
            db.execute("UPDATE saved SET value='after update'")
        self.assertEqual(self.run_driver("recover", self.root).stdout.strip(), "1")
        self.assertEqual((self.root / "native-rss/bin/kindle-rss").read_bytes(), b"old reader")
        self.assertFalse((self.root / "native-rss/docs/release-notes.md").exists())
        with sqlite3.connect(self.root / "native-rss/data/rss.sqlite") as db:
            self.assertEqual(db.execute("SELECT value FROM saved").fetchone()[0], "before update")
        self.assertEqual(self.run_driver("recover", self.root).stdout.strip(), "0")

    def test_commit_keeps_new_program_and_disables_rollback(self):
        self.stage()
        self.run_driver("apply", self.root)
        self.run_driver("commit", self.root)
        self.assertEqual(self.run_driver("recover", self.root).stdout.strip(), "0")
        self.assertEqual((self.root / "native-rss/bin/kindle-rss").read_bytes(), REQUIRED["native-rss/bin/kindle-rss"])

    def test_bad_archive_sha_does_not_change_install(self):
        self.stage(digest="0" * 64, ok=False)
        self.assertEqual((self.root / "native-rss/bin/kindle-rss").read_bytes(), b"old reader")

    def test_invalid_metadata_and_manifest_are_rejected_without_install_changes(self):
        cases = [
            (payload({"format": 1, "model": "KPW2", "version": VERSION}), "model"),
            (payload({"format": 2, "model": "KPW3", "version": VERSION}), "format"),
            (payload({"format": 1, "model": "KPW3", "version": "v999.0.1"}), "version"),
        ]
        for files, label in cases:
            with self.subTest(label=label):
                self.write_zip(files)
                self.stage(ok=False)
                self.assertEqual((self.root / "native-rss/bin/kindle-rss").read_bytes(), b"old reader")
        missing_metadata = payload()
        del missing_metadata["native-rss/UPDATE.json"]
        hashes = json.loads(missing_metadata["native-rss/MANIFEST.json"])["files"]
        del hashes["native-rss/UPDATE.json"]
        missing_metadata["native-rss/MANIFEST.json"] = json.dumps({"files": hashes}).encode()
        self.write_zip(missing_metadata)
        self.stage(ok=False)
        missing_file = payload()
        del missing_file["native-rss/fonts/reader.ttf"]
        hashes = json.loads(missing_file["native-rss/MANIFEST.json"])["files"]
        del hashes["native-rss/fonts/reader.ttf"]
        missing_file["native-rss/MANIFEST.json"] = json.dumps({"files": hashes}).encode()
        self.write_zip(missing_file)
        self.stage(ok=False)
        self.assertEqual((self.root / "native-rss/bin/kindle-rss").read_bytes(), b"old reader")

    def test_manifest_rejects_unlisted_missing_tampered_and_duplicate_zip_files(self):
        files = payload()
        self.write_zip(files, extra=(("native-rss/docs/unlisted.md", b"extra"),))
        self.stage(ok=False)
        files = payload()
        hashes = json.loads(files["native-rss/MANIFEST.json"])["files"]
        hashes["native-rss/docs/not-in-zip.md"] = hashlib.sha256(b"missing").hexdigest()
        files["native-rss/MANIFEST.json"] = json.dumps({"files": hashes}).encode()
        self.write_zip(files)
        self.stage(ok=False)
        files = payload()
        files["native-rss/fonts/reader.ttf"] = b"tampered"
        self.write_zip(files)
        self.stage(ok=False)
        files = payload()
        self.write_zip(files, duplicate=("native-rss/launch.sh", b"duplicate"))
        self.stage(ok=False)
        self.assertEqual((self.root / "native-rss/bin/kindle-rss").read_bytes(), b"old reader")

    def test_rejects_archive_path_traversal_absolute_paths_and_symlink_targets(self):
        files = payload()
        files["native-rss/../escape"] = b"escape"
        self.write_zip(files)
        self.stage(ok=False)
        files = payload()
        files["/tmp/update-escape"] = b"escape"
        self.write_zip(files)
        self.stage(ok=False)
        target = self.root / "native-rss/fonts/reader.ttf"
        target.parent.mkdir(parents=True)
        outside = self.base / "outside.ttf"
        outside.write_bytes(b"outside")
        target.symlink_to(outside)
        self.write_zip(payload())
        self.stage(ok=False)
        self.assertEqual(outside.read_bytes(), b"outside")
        self.assertEqual((self.root / "native-rss/bin/kindle-rss").read_bytes(), b"old reader")

    def test_cancel_leaves_old_install_and_no_ready_marker(self):
        self.stage(ok=False, cancel=True)
        self.assertEqual((self.root / "native-rss/bin/kindle-rss").read_bytes(), b"old reader")
        self.assertFalse((self.ready_dir() / "ready").exists())


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise RuntimeError("pass the compiled update_test driver path")
    DRIVER = str(Path(sys.argv.pop()).resolve())
    unittest.main()
