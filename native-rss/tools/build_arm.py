#!/usr/bin/env python3
"""Fetch pinned native sources, build static ARMv7 dependencies, and link Kindle RSS."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor"
BUILD = ROOT / "build-arm"
SOURCES = VENDOR / "arm-sources"
ARCHIVES = SOURCES / "archives"
UNPACKED = SOURCES / "unpacked"
PREFIX = BUILD / "prefix"
LOGS = BUILD / "logs"
ZIG = VENDOR / "zig-aarch64-macos-0.15.2" / "zig"
TARGET = "arm-linux-musleabi"
CPU = "cortex_a9"


def run(args: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None,
        log_name: str | None = None) -> None:
    label = " ".join(str(arg) for arg in args)
    print(f"  {label}", flush=True)
    if log_name:
        logfile = LOGS / log_name
        with logfile.open("w") as out:
            result = subprocess.run(args, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT)
        if result.returncode:
            print(f"Command failed; last output from {logfile}:", file=sys.stderr)
            try:
                print("\n".join(logfile.read_text(errors="replace").splitlines()[-60:]), file=sys.stderr)
            except OSError:
                pass
            raise subprocess.CalledProcessError(result.returncode, args)
    else:
        subprocess.run(args, cwd=cwd, env=env, check=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_inputs() -> dict[str, str]:
    paths = [*sorted((ROOT / "src").glob("*.cpp")), *sorted((ROOT / "src").glob("*.hpp")),
             *sorted((ROOT / "third_party").rglob("*.h")), ROOT / "CMakeLists.txt",
             ROOT / "tools/arm-sources.json", ROOT / "tools/reader-sources.json",
             ROOT / "tools/build_reader.py", ROOT / "tools/reader.cmake", Path(__file__).resolve()]
    return {str(path.relative_to(ROOT)): sha256(path) for path in paths}


def source_sha256(path: Path, source: dict[str, str]) -> str:
    kind = source.get("sha256_type", "archive")
    if kind == "archive":
        return sha256(path)
    if kind != "tar-content-v1":
        raise ValueError(f"Unknown source checksum type: {kind}")
    # Gitiles archives stamp each entry with the download time; verify files and modes instead.
    digest = hashlib.sha256()
    with tarfile.open(path) as archive:
        members = sorted(archive.getmembers(), key=lambda item: item.name)
        if len({member.name for member in members}) != len(members):
            raise ValueError("Duplicate source archive paths")
        for member in members:
            metadata = json.dumps([member.name, member.type.decode("latin1"), member.mode,
                                   member.size, member.linkname], separators=(",", ":")).encode()
            digest.update(len(metadata).to_bytes(8, "big"))
            digest.update(metadata)
            if member.isfile():
                with archive.extractfile(member) as data:
                    for block in iter(lambda: data.read(1024 * 1024), b""):
                        digest.update(block)
    return digest.hexdigest()


def download(name: str, source: dict[str, str]) -> Path:
    ARCHIVES.mkdir(parents=True, exist_ok=True)
    path = ARCHIVES / Path(source["url"]).name
    if path.exists():
        try:
            if source_sha256(path, source) == source["sha256"]:
                return path
        except (tarfile.TarError, EOFError, ValueError):
            pass
    path.unlink(missing_ok=True)
    partial = path.with_suffix(path.suffix + ".part")
    for attempt in range(1, 4):
        partial.unlink(missing_ok=True)
        try:
            run(["curl", "-L", "--fail", "--silent", "--show-error", "--retry", "2",
                 "--connect-timeout", "15", "--max-time", "300", "--proto", "=https", "--proto-redir", "=https",
                 "--retry-all-errors", "--output", str(partial), source["url"]],
                log_name=f"download-{name}.log")
            actual = source_sha256(partial, source)
            if actual != source["sha256"]:
                raise ValueError(f"SHA-256 mismatch for {name}: expected {source['sha256']}, got {actual}")
            partial.replace(path)
            return path
        except (OSError, subprocess.CalledProcessError, ValueError, tarfile.TarError, EOFError) as error:
            partial.unlink(missing_ok=True)
            if attempt == 3:
                raise RuntimeError(f"Unable to fetch verified {name} source after 3 clean attempts: {error}") from error
            print(f"  retrying {name} download ({attempt}/3): {error}", flush=True)
    raise AssertionError("unreachable")


def extract(archive: Path, target: Path) -> None:
    staging = target.with_name(target.name + ".extracting")
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    try:
        if zipfile.is_zipfile(archive):
            with zipfile.ZipFile(archive) as source:
                for member in source.infolist():
                    dest = (staging / member.filename).resolve()
                    if not dest.is_relative_to(staging.resolve()):
                        raise ValueError(f"Unsafe zip path: {member.filename}")
                source.extractall(staging)
        else:
            with tarfile.open(archive) as source:
                source.extractall(staging, filter="data")
        roots = list(staging.iterdir())
        if len(roots) == 1 and roots[0].is_dir():
            unpacked = roots[0]
        else:
            unpacked = staging
        shutil.rmtree(target, ignore_errors=True)
        unpacked.replace(target)
        if staging.exists():
            shutil.rmtree(staging)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def source_tree(name: str, spec: dict[str, str], archives: dict[str, Path]) -> Path:
    folder = UNPACKED / f"{name}-{spec['version']}"
    marker = folder / ".source-sha256"
    if marker.exists() and marker.read_text().strip() == spec["sha256"]:
        return folder
    extract(archives[name], folder)
    marker.write_text(spec["sha256"] + "\n")
    return folder


def write_toolchain() -> tuple[Path, Path, Path]:
    tool_dir = BUILD / "toolchain"
    tool_dir.mkdir(parents=True, exist_ok=True)
    cc, cxx, ar = tool_dir / "arm-cc", tool_dir / "arm-cxx", tool_dir / "arm-ar"
    for path, mode, args in ((cc, "cc", ("-target", TARGET, f"-mcpu={CPU}")),
                             (cxx, "c++", ("-target", TARGET, f"-mcpu={CPU}")),
                             (ar, "ar", ())):
        path.write_text("#!/bin/sh\nexec " + shlex.join([str(ZIG), mode, *args]) + ' "$@"\n')
        path.chmod(0o755)
    ranlib = tool_dir / "arm-ranlib"
    ranlib.write_text("#!/bin/sh\nexec " + shlex.join([str(ZIG), "ranlib"]) + ' "$@"\n')
    ranlib.chmod(0o755)
    toolchain = BUILD / "toolchain.cmake"
    toolchain.write_text(f"""set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7)
set(CMAKE_C_COMPILER "{cc}")
set(CMAKE_CXX_COMPILER "{cxx}")
set(CMAKE_AR "{ar}")
set(CMAKE_RANLIB "{ranlib}")
set(CMAKE_C_COMPILER_TARGET "{TARGET}")
set(CMAKE_CXX_COMPILER_TARGET "{TARGET}")
set(CMAKE_C_FLAGS_INIT "-Os -mcpu={CPU}")
set(CMAKE_CXX_FLAGS_INIT "-Os -mcpu={CPU}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH "{PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
""")
    return cc, cxx, toolchain


def cmake_build(name: str, source: Path, toolchain: Path, options: list[str], env: dict[str, str],
                *, targets: list[str] | None = None, install: bool = True) -> None:
    build_dir = BUILD / "deps" / name
    build_dir.mkdir(parents=True, exist_ok=True)
    run(["cmake", "-S", str(source), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_INSTALL_PREFIX={PREFIX}", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
         "-DBUILD_SHARED_LIBS=OFF", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", *options],
        env=env, log_name=f"{name}-configure.log")
    build_command = ["cmake", "--build", str(build_dir)]
    if targets:
        build_command.extend(["--target", *targets])
    build_command.extend(["--parallel", str(max(2, min(os.cpu_count() or 2, 8)))])
    run(build_command, env=env, log_name=f"{name}-build.log")
    if install:
        run(["cmake", "--install", str(build_dir)], env=env, log_name=f"{name}-install.log")


def build_sqlite(source: Path, cc: Path) -> None:
    amalgamation = source / "sqlite3.c"
    if not amalgamation.exists():
        candidates = list(source.rglob("sqlite3.c"))
        if len(candidates) != 1:
            raise RuntimeError("Could not identify SQLite amalgamation source")
        amalgamation = candidates[0]
    include = PREFIX / "include"
    lib = PREFIX / "lib"
    include.mkdir(parents=True, exist_ok=True)
    lib.mkdir(parents=True, exist_ok=True)
    build_dir = BUILD / "deps" / "sqlite"
    build_dir.mkdir(parents=True, exist_ok=True)
    object_file = build_dir / "sqlite3.o"
    run([str(cc), "-Os", "-DSQLITE_THREADSAFE=1", "-DSQLITE_DEFAULT_FOREIGN_KEYS=1",
         "-c", str(amalgamation), "-o", str(object_file)], log_name="sqlite-build.log")
    run([str(ZIG), "ar", "rcs", str(lib / "libsqlite3.a"), str(object_file)], log_name="sqlite-archive.log")
    sqlite_dir = amalgamation.parent
    for header in ("sqlite3.h", "sqlite3ext.h"):
        shutil.copy2(sqlite_dir / header, include / header)


def verify_elf(binary: Path) -> None:
    data = binary.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise RuntimeError("Output is not a 32-bit little-endian ELF")
    machine = struct.unpack_from("<H", data, 18)[0]
    if machine != 40:
        raise RuntimeError(f"ELF machine is {machine}, expected ARM (40)")
    elf_type, version = struct.unpack_from("<H", data, 16)[0], struct.unpack_from("<I", data, 20)[0]
    flags = struct.unpack_from("<I", data, 36)[0]
    if elf_type != 2 or version != 1 or data[6] != 1:
        raise RuntimeError("Output must be an ELF executable with version 1")
    if flags >> 24 != 5 or not flags & 0x200 or flags & 0x400:
        raise RuntimeError("Output must use ARM EABI5 soft-float ABI")
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    if phentsize != 32 or not phnum or phoff < 52 or phoff + phnum * phentsize > len(data):
        raise RuntimeError("Invalid ELF program header table")
    executable_load = False
    for i in range(phnum):
        p_type, offset, _, _, filesz, memsz, p_flags, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type in (2, 3):
            raise RuntimeError("Output has PT_DYNAMIC or PT_INTERP and is not fully static")
        if offset + filesz > len(data) or (p_type == 1 and filesz > memsz):
            raise RuntimeError("Invalid ELF segment bounds")
        executable_load |= p_type == 1 and bool(p_flags & 1) and filesz > 0
    if not executable_load:
        raise RuntimeError("Output has no executable load segment")
    print(f"Verified static ARM EABI5 soft-float ELF: {binary} ({len(data)} bytes)", flush=True)


def main() -> int:
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise RuntimeError("This pinned Zig archive targets the current Apple Silicon build host")
    if not ZIG.is_file():
        raise RuntimeError(f"Zig compiler not found at {ZIG}")
    inputs = build_inputs()
    for directory in (BUILD, LOGS, PREFIX, UNPACKED):
        directory.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / "tools" / "arm-sources.json").read_text())

    print("Verifying and preparing pinned ARM dependency sources", flush=True)
    archives = {name: download(name, spec) for name, spec in lock.items()}
    trees = {name: source_tree(name, spec, archives) for name, spec in lock.items()}
    cc, cxx, toolchain = write_toolchain()
    env = os.environ.copy()
    env.update({"CC": str(cc), "CXX": str(cxx), "AR": f"{ZIG} ar", "RANLIB": f"{ZIG} ranlib",
                "CFLAGS": f"-Os -mcpu={CPU}", "CXXFLAGS": f"-Os -mcpu={CPU}",
                "LDFLAGS": "-static", "PKG_CONFIG_DIR": "", "PKG_CONFIG_PATH": "",
                "PKG_CONFIG_LIBDIR": str(PREFIX / "lib" / "pkgconfig")})

    print("Building zlib", flush=True)
    cmake_build("zlib", trees["zlib"], toolchain, ["-DZLIB_BUILD_TESTING=OFF",
                                                       "-DZLIB_BUILD_SHARED=OFF", "-DZLIB_BUILD_STATIC=ON"], env)

    print("Building OpenSSL static ARM libraries", flush=True)
    openssl = trees["openssl"]
    run(["./Configure", "linux-armv4", "no-asm", "no-shared", "no-tests", "no-apps", "no-module",
         f"--prefix={PREFIX}", f"--openssldir={PREFIX / 'ssl'}"], cwd=openssl, env=env,
        log_name="openssl-configure.log")
    run(["make", "-j" + str(max(2, min(os.cpu_count() or 2, 8))), "build_libs"], cwd=openssl,
        env=env, log_name="openssl-build.log")
    run(["make", "install_sw"], cwd=openssl, env=env, log_name="openssl-install.log")

    print("Building SQLite amalgamation", flush=True)
    build_sqlite(trees["sqlite"], cc)

    print("Building libxml2 static without optional host dependencies", flush=True)
    cmake_build("libxml2", trees["libxml2"], toolchain, [
        "-DLIBXML2_WITH_TESTS=OFF", "-DLIBXML2_WITH_PROGRAMS=OFF", "-DLIBXML2_WITH_PYTHON=OFF",
        "-DLIBXML2_WITH_ICONV=OFF", "-DLIBXML2_WITH_LZMA=OFF", "-DLIBXML2_WITH_ZLIB=OFF"], env)

    print("Building FreeType static without optional decoders", flush=True)
    cmake_build("freetype", trees["freetype"], toolchain, [
        "-DFT_DISABLE_ZLIB=TRUE", "-DFT_DISABLE_BZIP2=TRUE", "-DFT_DISABLE_PNG=TRUE",
        "-DFT_DISABLE_HARFBUZZ=TRUE", "-DFT_DISABLE_BROTLI=TRUE", "-DFT_REQUIRE_ZLIB=FALSE",
        "-DFT_REQUIRE_BZIP2=FALSE", "-DFT_REQUIRE_PNG=FALSE", "-DFT_REQUIRE_HARFBUZZ=FALSE",
        "-DFT_REQUIRE_BROTLI=FALSE"], env)

    print("Building libcurl with OpenSSL and zlib only", flush=True)
    cmake_build("curl", trees["curl"], toolchain, [
        "-DBUILD_CURL_EXE=OFF", "-DBUILD_TESTING=OFF", "-DCURL_USE_OPENSSL=ON",
        "-DUSE_NGHTTP2=OFF",
        "-DOPENSSL_USE_STATIC_LIBS=TRUE", f"-DOPENSSL_ROOT_DIR={PREFIX}",
        "-DCURL_ZLIB=ON", f"-DZLIB_ROOT={PREFIX}", "-DCURL_BROTLI=OFF", "-DCURL_ZSTD=OFF",
        "-DCURL_USE_LIBPSL=OFF", "-DCURL_USE_LIBSSH2=OFF", "-DUSE_LIBIDN2=OFF",
        "-DCURL_USE_GSSAPI=OFF", "-DCURL_USE_LIBRTMP=OFF", "-DCURL_USE_MBEDTLS=OFF",
        "-DCURL_USE_WOLFSSL=OFF", "-DCURL_USE_GNUTLS=OFF", "-DCURL_USE_RUSTLS=OFF"], env)

    print("Building decoder-only libaom", flush=True)
    cmake_build("aom", trees["aom"], toolchain, [
        "-DAOM_TARGET_CPU=generic", "-DCONFIG_AV1_DECODER=1", "-DCONFIG_AV1_ENCODER=0",
        "-DCONFIG_AV1_HIGHBITDEPTH=1", "-DCONFIG_MULTITHREAD=0",
        "-DCONFIG_WEBM_IO=0", "-DENABLE_DOCS=OFF", "-DENABLE_TESTS=OFF",
        "-DENABLE_EXAMPLES=OFF", "-DENABLE_TOOLS=OFF"], env)

    print("Building libyuv without NEON, JPEG, tests, or tools", flush=True)
    yuv_env = env.copy()
    yuv_flags = f"-Os -mcpu={CPU} -DLIBYUV_DISABLE_NEON"
    yuv_env.update({"CFLAGS": yuv_flags, "CXXFLAGS": yuv_flags})
    cmake_build("libyuv", trees["libyuv"], toolchain, [
        "-DUNIT_TEST=OFF", "-DLIBYUV_DISABLE_JPEG=ON", f"-DCMAKE_C_FLAGS={yuv_flags}",
        f"-DCMAKE_CXX_FLAGS={yuv_flags}"], yuv_env, targets=["yuv"], install=False)
    (PREFIX / "lib").mkdir(parents=True, exist_ok=True)
    shutil.copy2(BUILD / "deps" / "libyuv" / "libyuv.a", PREFIX / "lib" / "libyuv.a")
    shutil.copytree(trees["libyuv"] / "include", PREFIX / "include", dirs_exist_ok=True)

    print("Building libavif with system decoder dependencies", flush=True)
    cmake_build("libavif", trees["libavif"], toolchain, [
        f"-DCMAKE_PREFIX_PATH={PREFIX}", "-DAVIF_CODEC_AOM=SYSTEM", "-DAVIF_CODEC_AOM_DECODE=ON",
        "-DAVIF_CODEC_AOM_ENCODE=OFF", "-DAVIF_CODEC_DAV1D=OFF", "-DAVIF_CODEC_LIBGAV1=OFF",
        "-DAVIF_CODEC_RAV1E=OFF", "-DAVIF_CODEC_SVT=OFF", "-DAVIF_CODEC_AVM=OFF",
        "-DAVIF_LIBYUV=SYSTEM", "-DAVIF_BUILD_APPS=OFF", "-DAVIF_BUILD_TESTS=OFF",
        "-DAVIF_JPEG=OFF", "-DAVIF_ZLIBPNG=OFF"], env)

    print("Linking native RSS statically for ARMv7 soft-float ABI", flush=True)
    cmake_build("webp", trees["webp"], toolchain, [
        "-DBUILD_SHARED_LIBS=OFF", "-DWEBP_BUILD_ANIM_UTILS=OFF", "-DWEBP_BUILD_CWEBP=OFF",
        "-DWEBP_BUILD_DWEBP=OFF", "-DWEBP_BUILD_GIF2WEBP=OFF", "-DWEBP_BUILD_IMG2WEBP=OFF",
        "-DWEBP_BUILD_VWEBP=OFF", "-DWEBP_BUILD_WEBPINFO=OFF", "-DWEBP_BUILD_WEBPMUX=OFF",
        "-DWEBP_BUILD_EXTRAS=OFF", "-DWEBP_USE_THREAD=OFF", "-DWEBP_ENABLE_SIMD=OFF"], env)
    from build_reader import build_app, sources as reader_sources
    binary = BUILD / "kindle-rss"
    compiled = build_app("arm", BUILD / "app", PREFIX, toolchain)
    shutil.copy2(compiled, binary)
    binary.chmod(0o755)
    verify_elf(binary)
    home_binary = BUILD / "kindle-rss-home"
    shutil.copy2(BUILD / "app/kindle-rss-home", home_binary)
    home_binary.chmod(0o755)
    verify_elf(home_binary)
    if build_inputs() != inputs:
        raise RuntimeError("Source inputs changed during the ARM build; rebuild before packaging")
    (BUILD / "build-info.json").write_text(json.dumps({
        "target": TARGET, "cpu": CPU, "zig": "0.15.2", "binary_sha256": sha256(binary),
        "home_bridge_sha256": sha256(home_binary),
        "inputs": inputs, "device_tested": False,
        "reader": {"name": "crengine", "revision": reader_sources()["cre"]["revision"],
                   "public_release_ready": False},
    }, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"ARM build failed: {error}", file=sys.stderr)
        raise SystemExit(1)
