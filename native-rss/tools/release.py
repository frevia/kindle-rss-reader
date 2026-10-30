#!/usr/bin/env python3
"""Validate and stage the installer and corresponding sources for release."""
import re
import json
import shutil
import sys
import hashlib
import tempfile
from xml.etree import ElementTree
from pathlib import Path
import zipfile

import package
import build_reader
import source_bundle


def verify_archives(device_path: Path, source_path: Path, tag: str):
    expected_reader = build_reader.verify_reader_license()
    with zipfile.ZipFile(device_path) as device, zipfile.ZipFile(source_path) as source:
        for archive in (device, source):
            if len(archive.namelist()) != len(set(archive.namelist())):
                raise ValueError("发布归档含重复文件")
        manifest = json.loads(device.read("native-rss/MANIFEST.json"))
        if manifest.get("public_release_ready") is not True:
            raise ValueError("安装包未通过许可检查，不能进入公开 Release")
        if manifest.get("reader") != expected_reader or manifest.get("distribution_license") != "GPL-3.0-only":
            raise ValueError("安装包不是已核对的 GPLv3 引擎构建")
        checksums = manifest["files"]
        if set(device.namelist()) != set(checksums) | {"native-rss/MANIFEST.json"}:
            raise ValueError("安装包文件与清单不一致")
        for name, digest in checksums.items():
            if hashlib.sha256(device.read(name)).hexdigest() != digest:
                raise ValueError(f"安装文件校验失败：{name}")
        build = json.loads(device.read("native-rss/BUILD.json"))
        if build.get("inputs") != package.build_inputs() or build.get("reader") != expected_reader:
            raise ValueError("构建输入与发布源码不一致")
        for name, key in (("kindle-rss", "binary_sha256"), ("kindle-rss-home", "home_bridge_sha256")):
            data = device.read("native-rss/bin/" + name)
            if hashlib.sha256(data).hexdigest() != build.get(key):
                raise ValueError("程序与构建清单不一致")
            with tempfile.TemporaryDirectory() as folder:
                binary = Path(folder) / name
                binary.write_bytes(data)
                package.verify_elf(binary)
        version = ElementTree.fromstring(device.read("extensions/native-rss/config.xml")).findtext("information/version")
        if version != tag[1:] or f"project(kindle_rss VERSION {tag[1:]} LANGUAGES CXX)" not in source.read("native-rss/CMakeLists.txt").decode():
            raise ValueError("发布标签与 CMake/KUAL 版本不一致")
        expected_sources = package.source_files(package.ROOT)
        expected_sources.update({name: (device.read(name), False) for name in checksums if name.startswith("native-rss/licenses/")})
        if set(source.namelist()) != set(expected_sources):
            raise ValueError("项目源码归档文件集合不完整或含额外文件")
        for name, (data, _) in expected_sources.items():
            if source.read(name) != data:
                raise ValueError(f"项目源码与当前发布不一致：{name}")
        if device.read("native-rss/COPYING") != source.read("COPYING"):
            raise ValueError("安装包和源码包的 GPLv3 条款不一致")


def stage_release(output: Path, tag: str) -> Path:
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ValueError("tag must be a stable version such as v0.0.2")

    files = (
        ("kindle-rss-kpw3-dev.zip", f"RSS-Reader-{tag}-KPW3.zip"),
        ("kindle-rss-source-dev.zip", f"RSS-Reader-{tag}-source.zip"),
        (source_bundle.BUNDLE_NAME, f"RSS-Reader-{tag}-third-party-sources.zip"),
    )
    sources = [(output / source, target) for source, target in files]
    for source, _ in sources:
        if not source.is_file():
            raise FileNotFoundError(f"Missing release input: {source}")
    verify_archives(sources[0][0], sources[1][0], tag)
    source_bundle.verify_bundle(sources[2][0])

    destination = output / f"release-{tag}"
    destination.mkdir(parents=True, exist_ok=True)
    targets = [(source, destination / name) for source, name in sources]
    for source, target in targets:
        shutil.copyfile(source, target)
    (destination / "SHA256SUMS.txt").write_text(
        "".join(f"{package.sha256(target)}  {target.name}\n" for _, target in targets),
        encoding="ascii",
    )
    return destination


def main(arguments=None):
    arguments = sys.argv[1:] if arguments is None else arguments
    if len(arguments) != 1 or not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", arguments[0]):
        raise SystemExit("Usage: release.py vMAJOR.MINOR.PATCH")
    tag = arguments[0]
    # The release tag belongs to this CLI, not package.py's option parser.
    arguments_before = sys.argv
    try:
        sys.argv = ["package.py"]
        package.main()
    finally:
        sys.argv = arguments_before
    source_bundle.build_bundle(Path(package.ROOT) / "output")
    print(stage_release(Path(package.ROOT) / "output", tag))


if __name__ == "__main__":
    main()
