import hashlib
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import package as pack
from build_arm_test import elf


class PackageTests(unittest.TestCase):
    def test_rejects_stale_or_changed_binary(self):
        with tempfile.TemporaryDirectory() as folder:
            binary=Path(folder)/"app";binary.write_bytes(elf())
            inputs={"src/main.cpp":"source-hash"}
            manifest={"inputs":inputs,"binary_sha256":hashlib.sha256(binary.read_bytes()).hexdigest()}
            pack.verify_build(binary,manifest,inputs)
            with self.assertRaises(RuntimeError):pack.verify_build(binary,manifest,{"src/main.cpp":"changed"})
            binary.write_bytes(b"changed")
            with self.assertRaises(RuntimeError):pack.verify_build(binary,manifest,inputs)

    def test_zip_is_repeatable_and_preserves_executable_mode(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"app.zip"
            files={"native-rss/launch.sh":(b"#!/bin/sh\n",True),"native-rss/README":(b"test",False)}
            pack.write_zip(path,files);first=path.read_bytes();pack.write_zip(path,files)
            self.assertEqual(first,path.read_bytes())
            with zipfile.ZipFile(path) as archive:
                self.assertEqual(archive.getinfo("native-rss/launch.sh").external_attr>>16&0o777,0o755)
                self.assertEqual(set(archive.namelist()),set(files))
            with self.assertRaises(RuntimeError):pack.write_zip(path,{"../escape":(b"x",False)})


if __name__=="__main__":unittest.main()
