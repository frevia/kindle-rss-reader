"""Reject incompatible or truncated artifacts before copying anything to Kindle."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("build_arm", Path(__file__).resolve().parents[1] / "tools/build_arm.py")
build_arm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_arm)


def elf():
    data = bytearray(88)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", data, 16, 2, 40, 1)
    struct.pack_into("<I", data, 28, 52)
    struct.pack_into("<I", data, 36, 0x05000200)
    struct.pack_into("<HHH", data, 40, 52, 32, 1)
    struct.pack_into("<8I", data, 52, 1, 84, 0x10000, 0, 4, 4, 5, 4)
    return data


class ArtifactTests(unittest.TestCase):
    def verify(self, data):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "app"
            path.write_bytes(data)
            build_arm.verify_elf(path)

    def test_static_softfloat(self):
        self.verify(elf())

    def test_rejects_invalid_artifacts(self):
        mutations = [
            ("hardfloat", 36, "I", 0x05000400),
            ("wrong machine", 18, "H", 62),
            ("shared object", 16, "H", 3),
            ("interpreter", 52, "I", 3),
            ("dynamic section", 52, "I", 2),
            ("bad header size", 42, "H", 0),
            ("missing headers", 44, "H", 0),
            ("header past EOF", 28, "I", 200),
            ("segment past EOF", 56, "I", 200),
            ("file exceeds memory", 72, "I", 3),
            ("not executable", 76, "I", 4),
        ]
        for name, offset, fmt, value in mutations:
            with self.subTest(name=name):
                data = elf()
                struct.pack_into("<" + fmt, data, offset, value)
                with self.assertRaises(RuntimeError):
                    self.verify(data)
        for size in (0, 4, 51, 60, 87):
            with self.subTest(truncated=size), self.assertRaises(RuntimeError):
                self.verify(elf()[:size])


if __name__ == "__main__":
    unittest.main()
