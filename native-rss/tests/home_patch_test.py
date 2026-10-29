"""Cross-check device patch reconstruction without distributing any firmware."""
import hashlib
import copy
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

    def test_arrow_margin_top_calibration_changes_only_constant(self):
        cases = (
            ("LoadConstUInt8", ("UInt8", False, 3), 0, 1),
            ("LoadConstDouble", ("Double", False, 13.5), 4.5, 2),
        )
        for load_op, old_constant, new_value, call_count in cases:
            with self.subTest(load_op=load_op):
                margin_top_id, width_id, height_id = 44, 45, 46
                value_reg = 2
                instructions = [
                    ("LoadConstUInt8", [("Reg8", False, 6), ("UInt8", False, 11)]),
                    ("PutNewOwnByIdShort", [("Reg8", False, 0), ("Reg8", False, 6), ("UInt8", True, width_id)]),
                    ("LoadConstUInt8", [("Reg8", False, 7), ("UInt8", False, 19)]),
                    ("PutNewOwnByIdShort", [("Reg8", False, 0), ("Reg8", False, 7), ("UInt8", True, height_id)]),
                    (load_op, [("Reg8", False, value_reg), old_constant]),
                ]
                for _ in range(call_count):
                    instructions.append(("Call2", [("Reg8", False, value_reg), ("Reg8", False, 8),
                                                    ("Reg8", False, 9), ("Reg8", False, value_reg)]))
                instructions.extend([
                    ("PutNewOwnByIdShort", [("Reg8", False, 0), ("Reg8", False, value_reg), ("UInt8", True, margin_top_id)]),
                    ("Ret", [("Reg8", False, 0)]),
                ])
                original = copy.deepcopy(instructions)
                before_bytes = hbc84.assemble(instructions)
                changed = patcher.calibrate_margin_top(instructions, margin_top_id,
                                                       old_constant[2], new_value, call_count)
                self.assertEqual(instructions, original)
                self.assertEqual(len(hbc84.assemble(changed)), len(before_bytes))
                self.assertEqual(changed[:4], original[:4])
                self.assertEqual(changed[5:], original[5:])
                self.assertEqual(changed[4][1][1][2], new_value)

    def test_arrow_margin_top_calibration_rejects_unverified_layouts(self):
        margin_id = 44
        base = [
            ("LoadConstUInt8", [("Reg8", False, 2), ("UInt8", False, 3)]),
            ("Call2", [("Reg8", False, 2), ("Reg8", False, 8), ("Reg8", False, 9), ("Reg8", False, 2)]),
            ("PutNewOwnByIdShort", [("Reg8", False, 0), ("Reg8", False, 2), ("UInt8", True, margin_id)]),
            ("Ret", [("Reg8", False, 0)]),
        ]
        invalid = []
        invalid.append(base[:1] + [("LoadConstUInt8", [("Reg8", False, 2), ("UInt8", False, 4)])] + base[1:])
        invalid.append([item for item in base if item != base[2]])
        invalid.append(base[:3] + [base[2], base[3]])
        invalid.append(base[:2] + [("PutNewOwnByIdShort", [("Reg8", False, 0), ("Reg8", False, 3), ("UInt8", True, margin_id)])] + base[3:])
        invalid.append(base[:1] + [("Call2", [("Reg8", False, 2), ("Reg8", False, 8), ("Reg8", False, 9), ("Reg8", False, 3)])] + base[2:])
        invalid.append(base[:1] + [("Jmp", [("Addr8", False, 0)])] + base[1:])
        for instructions in invalid:
            with self.subTest(instructions=instructions):
                with self.assertRaises(ValueError):
                    patcher.calibrate_margin_top(instructions, margin_id, 3, 0, 1)


if __name__ == "__main__": unittest.main()
