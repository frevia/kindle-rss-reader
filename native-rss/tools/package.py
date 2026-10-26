#!/usr/bin/env python3
"""Build a local development ZIP only; never copies files to a Kindle."""
import hashlib
import json
from pathlib import Path
import zipfile
import urllib.request

from build_arm import ROOT, BUILD, UNPACKED, VENDOR, build_inputs, sha256, verify_elf


def verify_build(binary, manifest, inputs):
    if manifest.get("inputs") != inputs or manifest.get("binary_sha256") != sha256(binary):
        raise RuntimeError("ARM 产物与当前源码不一致，请重新运行 tools/build_arm.py")
    verify_elf(binary)


def write_zip(path, files):
    temporary = path.with_suffix(".zip.tmp")
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, (data, executable) in sorted(files.items()):
            if name.startswith("/") or ".." in Path(name).parts:
                raise RuntimeError("无效打包路径")
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (0o100755 if executable else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    temporary.replace(path)


def main():
    binary = BUILD / "kindle-rss"
    manifest = json.loads((BUILD / "build-info.json").read_text())
    verify_build(binary, manifest, build_inputs())
    runtime = json.loads((ROOT / "tools/runtime-sources.json").read_text())
    for name, spec in runtime.items():
        path = VENDOR / "runtime" / name
        if not path.exists():
            with urllib.request.urlopen(spec["url"], timeout=30) as response:
                data = response.read(25*1024*1024+1)
            if len(data)>25*1024*1024 or hashlib.sha256(data).hexdigest()!=spec["sha256"]:
                raise RuntimeError(f"运行资源下载校验失败：{name}")
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary = path.with_suffix(path.suffix+".tmp")
            temporary.write_bytes(data)
            temporary.replace(path)
        if sha256(VENDOR / "runtime" / name) != spec["sha256"]:
            raise RuntimeError(f"运行资源校验失败：{name}")
    files = {}
    def add(name, source, executable=False):
        if source.is_symlink():
            raise RuntimeError(f"拒绝打包符号链接：{source}")
        files[name] = (source.read_bytes(), executable)
    add("native-rss/bin/kindle-rss", binary, True)
    add("native-rss/book-cover.png", ROOT / "device/book-cover.png")
    add("native-rss/launch.sh", ROOT / "device/launch.sh", True)
    add("native-rss/diagnose.sh", ROOT / "device/diagnose.sh", True)
    add("documents/RSS设备诊断.sh", ROOT / "device/diagnose.sh", True)
    add("documents/RSS阅读器.sh", ROOT / "device/reader-scriptlet.sh", True)
    add("native-rss/fonts/reader.ttf", VENDOR / "runtime/reader.ttf")
    add("native-rss/certs/ca-bundle.pem", VENDOR / "runtime/ca-bundle.pem")
    for name in ("config.xml", "menu.json"):
        add("extensions/native-rss/" + name, ROOT / "device/kual" / name)
    add("native-rss/INSTALL.txt", ROOT / "device/INSTALL.txt")
    add("native-rss/README.md", ROOT / "README.md")
    for document in sorted((ROOT / "docs").iterdir()):
        if document.suffix not in (".md", ".txt"):
            continue
        add("native-rss/docs/" + document.name, document)
    add("native-rss/BUILD.json", BUILD / "build-info.json")
    add("native-rss/licenses/runtime-sources.json", ROOT / "tools/runtime-sources.json")
    add("native-rss/licenses/arm-sources.json", ROOT / "tools/arm-sources.json")
    licenses = {
        "libwebp.txt": UNPACKED / "webp-1.6.0/COPYING",
        "libwebp-PATENTS.txt": UNPACKED / "webp-1.6.0/PATENTS",
        "libavif.txt": UNPACKED / "libavif-1.4.2/LICENSE",
        "libaom.txt": UNPACKED / "aom-3.13.2/LICENSE",
        "libaom-PATENTS.txt": UNPACKED / "aom-3.13.2/PATENTS",
        "libyuv.txt": UNPACKED / "libyuv-47ba8e2160b71a7cf95c0720cd8944ab771e533e/LICENSE",
        "libyuv-PATENTS.txt": UNPACKED / "libyuv-47ba8e2160b71a7cf95c0720cd8944ab771e533e/PATENTS",
        "OpenSSL.txt": UNPACKED / "openssl-3.5.9/LICENSE.txt",
        "curl.txt": UNPACKED / "curl-8.23.0/COPYING",
        "libxml2.txt": UNPACKED / "libxml2-2.15.4/Copyright",
        "zlib.txt": UNPACKED / "zlib-1.3.2/LICENSE",
        "FreeType.txt": UNPACKED / "freetype-2.15.0/LICENSE.TXT",
        "FreeType-FTL.txt": UNPACKED / "freetype-2.15.0/docs/FTL.TXT",
        "FreeType-BDF.txt": UNPACKED / "freetype-2.15.0/src/bdf/README",
        "FreeType-PCF.txt": UNPACKED / "freetype-2.15.0/src/pcf/README",
        "stb.txt": ROOT / "third_party/stb/LICENSE",
        "Noto-OFL.txt": VENDOR / "runtime/OFL.txt",
        "Mozilla-MPL-2.0.txt": VENDOR / "runtime/MPL-2.0.txt",
        "musl.txt": VENDOR / "zig-aarch64-macos-0.15.2/lib/libc/musl/COPYRIGHT",
        "libcxx.txt": VENDOR / "zig-aarch64-macos-0.15.2/lib/libcxx/LICENSE.TXT",
        "libcxxabi.txt": VENDOR / "zig-aarch64-macos-0.15.2/lib/libcxxabi/LICENSE.TXT",
        "libunwind.txt": VENDOR / "zig-aarch64-macos-0.15.2/lib/libunwind/LICENSE.TXT",
        "Zig.txt": VENDOR / "zig-aarch64-macos-0.15.2/LICENSE",
    }
    for name, source in licenses.items():
        add("native-rss/licenses/" + name, source)
    add("native-rss/licenses/pinyin-Apache-2.0.txt", ROOT / "third_party/pinyin/LICENSE")
    add("native-rss/licenses/pinyin-SOURCE.json", ROOT / "third_party/pinyin/SOURCE.json")
    files["native-rss/licenses/SQLite.txt"] = ("SQLite is in the public domain.\nhttps://www.sqlite.org/copyright.html\n".encode(), False)
    files["native-rss/licenses/NOTICE.txt"] = (
        b"Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.\n"
        b"FreeType is used under the FreeType License. stb_image is used under its MIT license.\n"
        b"The bundled Noto Sans SC font is unmodified and licensed under SIL OFL 1.1.\n"
        b"The unmodified Mozilla CA bundle is obtained from curl.se and is covered by MPL 2.0.\n"
        b"libavif, libaom, and libyuv are distributed under their included BSD licenses and patent notices.\n", False)
    checksums = {name: hashlib.sha256(data).hexdigest() for name, (data, _) in files.items()}
    files["native-rss/MANIFEST.json"] = (json.dumps({"development_only": True, "device_tested": False, "files": checksums}, indent=2).encode(), False)
    output = ROOT / "output"
    output.mkdir(exist_ok=True)
    target = output / "kindle-rss-kpw3-dev.zip"
    write_zip(target, files)
    source_files = {}
    for folder in ("src", "tests", "tools", "device", "docs", "third_party"):
        for path in sorted((ROOT / folder).rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                if path.is_symlink():
                    raise RuntimeError(f"拒绝打包符号链接：{path}")
                source_files["native-rss/" + str(path.relative_to(ROOT))] = (path.read_bytes(), path.suffix == ".sh")
    for name in ("README.md", "CMakeLists.txt", ".gitignore"):
        source_files["native-rss/" + name] = ((ROOT / name).read_bytes(), False)
    write_zip(output / "kindle-rss-source-dev.zip", source_files)
    print(f"Development package: {target} ({target.stat().st_size} bytes)")
    print(f"SHA-256: {sha256(target)}")


if __name__ == "__main__":
    main()
