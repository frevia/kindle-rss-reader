"""Build the device self-test with assertions, including in optimized builds."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
subprocess.run([
    str(ROOT / "build-arm/toolchain/arm-cxx"),
    "-std=c++17", "-Os", "-UNDEBUG", "-static", "-I", str(ROOT / "src"),
    str(ROOT / "src/process.cpp"), str(ROOT / "tests/process_test.cpp"),
    "-o", str(ROOT / "build-arm/process_test"),
], check=True)
