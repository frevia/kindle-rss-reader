import hashlib
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import release
import release_notes
from build_arm_test import elf

TAG = "v0.0.3"


class ReleaseTests(unittest.TestCase):
    def test_notes_use_version_document_and_fixed_commit_links(self):
        sha = "a" * 40
        rendered = release_notes.render(TAG, sha)
        self.assertIn("RSS Reader v0.0.3", rendered)
        self.assertIn(f"/blob/{sha}/COPYING", rendered)
        self.assertNotIn("](../../COPYING)", rendered)
        with self.assertRaises(ValueError): release_notes.render("../escape", sha)
        with self.assertRaises(ValueError): release_notes.render(TAG, "main")

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.output = Path(self.folder.name)
        self.inputs = {"src/main.cpp": "source-hash"}
        self.reader = release.build_reader.verify_reader_license()
        self.sources = {
            "COPYING": (b"GPLv3 fixture", False),
            "native-rss/CMakeLists.txt": (b"project(kindle_rss VERSION 0.0.3 LANGUAGES CXX)", False),
        }
        for obj, name, value in ((release.package, "build_inputs", self.inputs),
                                 (release.package, "source_files", self.sources)):
            mock = patch.object(obj, name, return_value=value)
            mock.start(); self.addCleanup(mock.stop)
        self.files = {
            "native-rss/bin/kindle-rss": elf(),
            "native-rss/bin/kindle-rss-home": elf(),
            "native-rss/COPYING": b"GPLv3 fixture",
            "extensions/native-rss/config.xml": b"<extension><information><version>0.0.3</version></information></extension>",
        }
        self.build = {"inputs": self.inputs, "reader": self.reader,
                      "binary_sha256": hashlib.sha256(elf()).hexdigest(),
                      "home_bridge_sha256": hashlib.sha256(elf()).hexdigest()}
        self.manifest = {"public_release_ready": True, "reader": self.reader,
                         "distribution_license": "GPL-3.0-only"}
        self.bundle = self.output / release.source_bundle.BUNDLE_NAME
        self.bundle.write_bytes(b"verified separately by source_bundle_test")
        self.write_archives()

    def write_archives(self):
        self.files["native-rss/BUILD.json"] = json.dumps(self.build).encode()
        self.manifest["files"] = {name: hashlib.sha256(data).hexdigest() for name, data in self.files.items()}
        with zipfile.ZipFile(self.output / "kindle-rss-kpw3-dev.zip", "w") as z:
            for name, data in self.files.items(): z.writestr(name, data)
            z.writestr("native-rss/MANIFEST.json", json.dumps(self.manifest))
        with zipfile.ZipFile(self.output / "kindle-rss-source-dev.zip", "w") as z:
            for name, (data, _) in self.sources.items(): z.writestr(name, data)

    def test_stages_all_three_verified_archives_and_checksums(self):
        with patch.object(release.source_bundle, "verify_bundle") as verify:
            destination = release.stage_release(self.output, TAG)
            verify.assert_called_once_with(self.bundle)
        self.assertEqual(destination, self.output / "release-v0.0.3")
        names = ["RSS-Reader-v0.0.3-KPW3.zip", "RSS-Reader-v0.0.3-source.zip", "RSS-Reader-v0.0.3-third-party-sources.zip"]
        self.assertEqual((destination / "SHA256SUMS.txt").read_text(),
                         "".join(f"{release.package.sha256(destination / name)}  {name}\n" for name in names))

    def test_boolean_alone_does_not_authorize_release(self):
        for field, value in (("public_release_ready", False), ("public_release_ready", "true"),
                             ("reader", {"name": "crengine"}), ("distribution_license", "MIT")):
            with self.subTest(field=field, value=value):
                previous = self.manifest[field]
                self.manifest[field] = value; self.write_archives()
                with self.assertRaises(ValueError): release.stage_release(self.output, TAG)
                self.assertFalse((self.output / "release-v0.0.3").exists())
                self.manifest[field] = previous

    def test_stale_build_and_wrong_tag_are_rejected(self):
        self.build["inputs"] = {}; self.write_archives()
        with self.assertRaisesRegex(ValueError, "构建输入"): release.stage_release(self.output, TAG)
        self.build["inputs"] = self.inputs; self.write_archives()
        with self.assertRaisesRegex(ValueError, "版本"): release.stage_release(self.output, "v0.0.4")

    def test_missing_or_unverified_dependency_sources_block_release(self):
        with patch.object(release.source_bundle, "verify_bundle", side_effect=ValueError("bad sources")):
            with self.assertRaisesRegex(ValueError, "bad sources"): release.stage_release(self.output, TAG)
        self.assertFalse((self.output / "release-v0.0.3").exists())
        self.bundle.unlink()
        with self.assertRaises(FileNotFoundError): release.stage_release(self.output, TAG)

    def test_source_mismatch_and_tampered_binary_are_rejected(self):
        self.sources["COPYING"] = (b"changed after packaging", False)
        with self.assertRaisesRegex(ValueError, "项目源码"): release.stage_release(self.output, TAG)
        self.sources["COPYING"] = (b"GPLv3 fixture", False)
        self.files["native-rss/bin/kindle-rss"] = b"tampered"; self.write_archives()
        with self.assertRaisesRegex(ValueError, "程序与构建"): release.stage_release(self.output, TAG)

    def test_invalid_tag_rejected_before_packaging(self):
        for tag in ("", "0.0.3", "v0.0.3-rc1", "v1.2", "v1.2.3/escape", "v1.2.3 "):
            with patch.object(release.package, "main") as build:
                with self.assertRaises(SystemExit): release.main([tag])
                build.assert_not_called()
            with self.assertRaises(ValueError): release.stage_release(self.output, tag)

    def test_release_cli_does_not_pass_tag_to_package_parser(self):
        with patch.object(sys, "argv", ["release.py", TAG]), \
                patch.object(release.package, "main", side_effect=lambda: self.assertEqual(sys.argv, ["package.py"])), \
                patch.object(release.source_bundle, "build_bundle"), \
                patch.object(release, "stage_release"):
            release.main()
            self.assertEqual(sys.argv, ["release.py", TAG])


if __name__ == "__main__":
    unittest.main()
