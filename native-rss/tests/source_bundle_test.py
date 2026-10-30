#!/usr/bin/env python3
"""Offline checks for source bundle reproducibility and lock verification."""

from __future__ import annotations

import hashlib
import io
import json
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import source_bundle


class SourceBundleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.archives_patch = patch.object(source_bundle.build_arm, "ARCHIVES", self.root / "archives")
        self.archives_patch.start()
        source_bundle.build_arm.ARCHIVES.mkdir()
        self.archive_data = {}
        self.locks = {
            "arm": {"libyuv": {"version": "1.2", "url": "https://upstream.test/libyuv.tar.gz",
                                 "sha256_type": "tar-content-v1"}},
            "reader": {"engine": {"repo": "org/engine", "revision": "abc123"}},
            "toolchain": {"zig": {"version": "0.15.2", "url": "https://upstream.test/zig.tar.xz"}},
        }
        for group, entries in self.locks.items():
            for name, spec in entries.items():
                spec.setdefault("url", f"https://upstream.test/{group}-{name}.tar.gz")
                if spec.get("sha256_type") == "tar-content-v1":
                    buffer = io.BytesIO()
                    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
                        content = b"small synthetic source tree\n"
                        member = tarfile.TarInfo("libyuv/README")
                        member.size = len(content)
                        archive.addfile(member, io.BytesIO(content))
                    data = buffer.getvalue()
                else:
                    data = f"synthetic {group}/{name} source archive\n".encode()
                self.archive_data[spec["url"]] = data
                if spec.get("sha256_type") == "tar-content-v1":
                    archive_path = self.root / "synthetic-libyuv.tar.gz"
                    archive_path.write_bytes(data)
                    spec["sha256"] = source_bundle.build_arm.source_sha256(archive_path, spec)
                else:
                    spec["sha256"] = hashlib.sha256(data).hexdigest()
        self.lock_paths = {}
        for group in self.locks:
            path = self.root / f"{group}.json"
            path.write_text(json.dumps(self.locks[group]))
            self.lock_paths[group] = path
        self.locks_patch = patch.object(source_bundle, "LOCKS", self.lock_paths)
        self.locks_patch.start()
        self.download_patch = patch.object(source_bundle.build_arm, "download", self._download)
        self.download_patch.start()

    def tearDown(self) -> None:
        self.archives_patch.stop()
        self.download_patch.stop()
        self.locks_patch.stop()
        self.temp.cleanup()

    def _download(self, name: str, spec: dict[str, str]) -> Path:
        path = source_bundle.build_arm.ARCHIVES / spec["url"].split("/")[-1]
        path.write_bytes(self.archive_data[spec["url"]])
        return path

    def _build(self, folder: str = "out") -> Path:
        return source_bundle.build_bundle(self.root / folder)

    def _rewrite(self, source: Path, destination: Path, *, omit: str | None = None,
                 duplicate: str | None = None, corrupt: str | None = None) -> None:
        with zipfile.ZipFile(source) as original, zipfile.ZipFile(destination, "w") as rewritten:
            for item in original.infolist():
                if item.filename == omit:
                    continue
                data = original.read(item.filename)
                rewritten.writestr(item, b"changed archive bytes" if item.filename == corrupt else data)
                if item.filename == duplicate:
                    rewritten.writestr(item, data)

    def test_build_is_deterministic_and_verifies(self) -> None:
        first = self._build("first")
        second = self._build("second")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        manifest = source_bundle.verify_bundle(first)
        self.assertEqual(len(manifest["sources"]), 3)
        self.assertEqual({item["group"] for item in manifest["sources"]}, set(self.locks))

    def test_missing_member_rejected(self) -> None:
        bundle = self._build()
        manifest = source_bundle.verify_bundle(bundle)
        member = manifest["sources"][0]["member"]
        altered = self.root / "missing.zip"
        self._rewrite(bundle, altered, omit=member)
        with self.assertRaisesRegex(ValueError, "member set mismatch"):
            source_bundle.verify_bundle(altered)

    def test_corrupt_member_rejected(self) -> None:
        bundle = self._build()
        manifest = source_bundle.verify_bundle(bundle)
        member = manifest["sources"][0]["member"]
        altered = self.root / "corrupt.zip"
        self._rewrite(bundle, altered, corrupt=member)
        with self.assertRaisesRegex(ValueError, "byte SHA-256 mismatch"):
            source_bundle.verify_bundle(altered)

    def test_duplicate_member_rejected(self) -> None:
        bundle = self._build()
        with zipfile.ZipFile(bundle) as original:
            member = original.namelist()[0]
        altered = self.root / "duplicate.zip"
        self._rewrite(bundle, altered, duplicate=member)
        with self.assertRaisesRegex(ValueError, "Duplicate ZIP member"):
            source_bundle.verify_bundle(altered)

    def test_changed_lock_rejected(self) -> None:
        bundle = self._build()
        self.locks["reader"]["engine"]["revision"] = "changed-revision"
        self.lock_paths["reader"].write_text(json.dumps(self.locks["reader"]))
        with self.assertRaises(ValueError):
            source_bundle.verify_bundle(bundle)


if __name__ == "__main__":
    unittest.main()
