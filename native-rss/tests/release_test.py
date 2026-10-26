import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import release


class ReleaseTests(unittest.TestCase):
    def test_stages_named_archives_without_changing_inputs_and_writes_checksums(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            contents = {
                "kindle-rss-kpw3-dev.zip": b"device zip bytes",
                "kindle-rss-source-dev.zip": b"source zip bytes",
            }
            for name, data in contents.items():
                (output / name).write_bytes(data)

            destination = release.stage_release(output, "v0.0.2")

            expected = {
                "RSS-Reader-v0.0.2-KPW3.zip": contents["kindle-rss-kpw3-dev.zip"],
                "RSS-Reader-v0.0.2-source.zip": contents["kindle-rss-source-dev.zip"],
            }
            self.assertEqual(destination, output / "release-v0.0.2")
            for name, data in expected.items():
                self.assertEqual((destination / name).read_bytes(), data)
            for name, data in contents.items():
                self.assertEqual((output / name).read_bytes(), data)
            self.assertEqual(
                (destination / "SHA256SUMS.txt").read_text(encoding="ascii"),
                "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in expected.items()),
            )

    def test_invalid_tag_is_rejected_before_package_or_filesystem_changes(self):
        for tag in ("", "0.0.2", "v0.0.2-rc1", "v1.2", "v1.2.3/escape", "v1.2.3 "):
            with self.subTest(tag=tag), tempfile.TemporaryDirectory() as folder:
                with patch.object(release.package, "main") as build:
                    with self.assertRaises(SystemExit):
                        release.main([tag])
                build.assert_not_called()
                with self.assertRaises(ValueError):
                    release.stage_release(Path(folder), tag)
                self.assertEqual(list(Path(folder).iterdir()), [])

    def test_missing_input_zip_raises_without_creating_release_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            (output / "kindle-rss-kpw3-dev.zip").write_bytes(b"zip")
            with self.assertRaises(FileNotFoundError):
                release.stage_release(output, "v0.0.2")
            self.assertEqual(list(output.iterdir()), [output / "kindle-rss-kpw3-dev.zip"])


if __name__ == "__main__":
    unittest.main()
