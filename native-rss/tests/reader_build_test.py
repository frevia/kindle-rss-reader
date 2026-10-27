"""Check the pinned reader build contract without networking or compiling dependencies."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build_arm
import build_reader


class ReaderBuildTests(unittest.TestCase):
    def test_reader_configuration_is_part_of_artifact_identity(self):
        inputs = build_arm.build_inputs()
        for name in ("CMakeLists.txt", "tools/reader.cmake", "tools/reader-sources.json", "tools/build_reader.py"):
            self.assertEqual(inputs[name], build_arm.sha256(build_arm.ROOT / name))

    def test_invalid_target_or_toolchain_is_rejected_before_creating_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for target, toolchain in (("other", None), ("arm", None), ("host", root / "toolchain")):
                with self.assertRaises(ValueError):
                    build_reader.build_app(target, root / "build", root / "prefix", toolchain)
            self.assertEqual(list(root.iterdir()), [])

    def test_prepared_sources_reuse_only_the_matching_verified_revision(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            archive = root / "source.tar.gz"
            archive.write_bytes(b"fixture")
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            tree = root / "source"
            spec = {"cre": {"repo": "koreader/crengine", "revision": "pinned", "sha256": digest}}
            def extract(_archive, destination):
                destination.mkdir(exist_ok=True)
            with patch.object(build_reader, "sources", return_value=spec), \
                    patch.object(build_reader, "source_path", return_value=tree), \
                    patch.object(build_reader.arm, "download", return_value=archive) as download, \
                    patch.object(build_reader.arm, "extract", side_effect=extract) as unpack:
                self.assertEqual(build_reader.prepare_sources(("cre",)), {"cre": tree})
                build_reader.prepare_sources(("cre",))
                self.assertEqual(unpack.call_count, 1)
                download.assert_called_with("reader-cre", {"url": "https://codeload.github.com/koreader/crengine/tar.gz/pinned", "sha256": digest})
                (tree / ".verified-source").write_text("stale")
                build_reader.prepare_sources(("cre",))
                self.assertEqual(unpack.call_count, 2)
                self.assertEqual((tree / ".verified-source").read_text().strip(), digest)


if __name__ == "__main__":
    unittest.main()
