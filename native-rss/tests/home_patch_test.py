"""Cross-check device patch reconstruction without distributing any firmware."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_home_patch as patcher
import hbc84

BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path(os.environ.get("HOME_BRIDGE_BIN", ROOT / "build/kindle-rss-home")).resolve()


class HomePatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name)
        self.source = bytes(range(256)) * 20
        self.result = b"own prefix" + self.source[:2000] + b"new hook" + self.source[3000:]
        self.recipe = patcher.make_recipe(self.source, self.result)
        self.original = self.folder / "original"
        self.patch = self.folder / "patch"
        self.output = self.folder / "output"
        self.original.write_bytes(self.source)
        self.patch.write_bytes(self.recipe)

    def tearDown(self): self.temp.cleanup()

    def run_patch(self):
        return subprocess.run([str(BINARY), "patch", str(self.original), str(self.patch), str(self.output)], capture_output=True, timeout=5)

    def test_device_and_host_reconstruct_identically(self):
        self.assertEqual(patcher.apply_recipe(self.source, self.recipe), self.result)
        self.assertEqual(self.run_patch().returncode, 0)
        self.assertEqual(self.output.read_bytes(), self.result)
        self.assertEqual(self.original.read_bytes(), self.source)
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o600)

    def test_corrupt_or_truncated_recipe_does_not_replace_existing_output(self):
        self.output.write_bytes(b"preserved")
        corruptions = [self.recipe[:80], self.recipe + b"extra", self.recipe[:40] + bytes(32) + self.recipe[72:],
                       self.recipe[:80] + struct.pack("<I", 100001) + self.recipe[84:],
                       self.recipe[:84] + b"\0" + struct.pack("<II", 0xffffffff, 0xffffffff)]
        for data in corruptions:
            with self.subTest(size=len(data)):
                self.patch.write_bytes(data)
                self.assertNotEqual(self.run_patch().returncode, 0)
                self.assertEqual(self.output.read_bytes(), b"preserved")
                self.assertEqual(self.original.read_bytes(), self.source)

    def test_wrong_original_and_aliases_are_rejected(self):
        self.original.write_bytes(b"different")
        self.assertNotEqual(self.run_patch().returncode, 0)
        self.assertFalse(self.output.exists())
        self.original.write_bytes(self.source)
        for link in (lambda: self.output.symlink_to(self.original), lambda: os.link(self.original, self.output)):
            link()
            self.assertNotEqual(self.run_patch().returncode, 0)
            self.assertEqual(self.original.read_bytes(), self.source)
            self.output.unlink()

    def test_unsupported_firmware_fails_before_invoking_a_compiler(self):
        output = self.folder / "not-created"
        process = subprocess.run([sys.executable, str(ROOT / "tools/build_home_patch.py"), "--original", str(self.original),
                                  "--hermesc", str(self.folder / "missing-compiler"), "--output-dir", str(output)], capture_output=True, timeout=5)
        self.assertNotEqual(process.returncode, 0)
        self.assertFalse(output.exists())

    def test_hbc_operand_roundtrip_and_strict_bounds(self):
        instructions = [("LoadConstUndefined", [("Reg8", False, 0)]), ("Ret", [("Reg8", False, 0)])]
        self.assertEqual(hbc84.disassemble(hbc84.assemble(instructions)), instructions)
        with self.assertRaises(ValueError): hbc84.disassemble(b"\xff")
        with self.assertRaises(ValueError): hbc84.HBC84(bytes(160))
        with self.assertRaises(ValueError): hbc84.assemble([("GetById", [("UInt8", False, 0)])])


if __name__ == "__main__": unittest.main()
