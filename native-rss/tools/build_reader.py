#!/usr/bin/env python3
"""Build the pinned reading engine and the ordinary RSS application."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import sys

import build_arm as arm

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "tools/reader-sources.json"


def sources():
    return json.loads(LOCK.read_text())


def source_path(name):
    return ROOT / "vendor/reader-sources" / (name + "-" + sources()[name]["revision"])


def prepare_sources(names):
    trees = {}
    for name in names:
        item = sources()[name]
        archive = arm.download("reader-" + name, {
            "url": f"https://codeload.github.com/{item['repo']}/tar.gz/{item['revision']}",
            "sha256": item["sha256"],
        })
        tree = source_path(name)
        marker = tree / ".verified-source"
        if not marker.is_file() or marker.read_text().strip() != item["sha256"]:
            arm.extract(archive, tree)
            marker.write_text(item["sha256"] + "\n")
        trees[name] = tree
    return trees


def pc(prefix, name, version, library):
    folder = prefix / "lib/pkgconfig"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / (name + ".pc")).write_text(
        f"prefix={prefix}\nName: {name}\nDescription: RSS reading dependency\n"
        f"Version: {version}\nLibs: -L${{prefix}}/lib -l{library}\nCflags: -I${{prefix}}/include\n")


def build_app(target, build, prefix, toolchain=None):
    if target not in ("host", "arm") or (target == "arm") != bool(toolchain):
        raise ValueError("ARM requires a toolchain; host must use the host compiler")
    build, prefix = Path(build).resolve(), Path(prefix).resolve()
    work = build.parent / "reader-deps"
    for folder in (work, prefix / "include", prefix / "lib", arm.LOGS):
        folder.mkdir(parents=True, exist_ok=True)
    trees = prepare_sources(sources() if target == "arm" else ("cre", "xxhash", "unibreak"))
    env = os.environ.copy()
    if target == "arm":
        env.update(PKG_CONFIG_PATH="", PKG_CONFIG_DIR="", PKG_CONFIG_LIBDIR=str(prefix / "lib/pkgconfig"))
        cc = Path(toolchain).parent / "toolchain/arm-cc"
        archiver = [str(arm.ZIG), "ar"]
        version = re.search(r'#define SQLITE_VERSION\s+"([^"]+)"', (prefix / "include/sqlite3.h").read_text())
        if not version:
            raise RuntimeError("SQLite headers are missing a version")
        pc(prefix, "sqlite3", version.group(1), "sqlite3")
    else:
        cc, archiver = "cc", ["ar"]
        env["PKG_CONFIG_PATH"] = os.pathsep.join([
            str(prefix / "lib/pkgconfig"), str(ROOT / "build-host-media/prefix/lib/pkgconfig"),
            env.get("PKG_CONFIG_PATH", ""),
        ])

    def archive(name, paths, version, headers):
        objects = []
        folder = work / name
        folder.mkdir(exist_ok=True)
        for path in paths:
            obj = folder / (path.stem + ".o")
            arm.run([str(cc), "-Os", "-I", str(path.parent), "-c", str(path), "-o", str(obj)])
            objects.append(str(obj))
        arm.run([*archiver, "rcs", str(prefix / "lib" / ("lib" + name + ".a")), *objects])
        for header in headers:
            shutil.copy2(header, prefix / "include" / header.name)
        pc(prefix, "lib" + name, version, name)

    xx = trees["xxhash"]
    archive("xxhash", [xx / "xxhash.c"], sources()["xxhash"]["revision"].lstrip("v"), [xx / "xxhash.h"])
    uni = trees["unibreak"] / "src"
    units = "unibreakbase unibreakdef linebreak linebreakdata linebreakdef eastasianwidthdef emojidef graphemebreak wordbreak".split()
    version = sources()["unibreak"]["revision"].removeprefix("libunibreak_").replace("_", ".")
    archive("unibreak", [uni / (unit + ".c") for unit in units], version, list(uni.glob("*.h")))

    if target == "arm":
        for name, options in (
            ("png", ["-DPNG_SHARED=OFF", "-DPNG_STATIC=ON", "-DPNG_TESTS=OFF", "-DPNG_EXECUTABLES=OFF"]),
            ("harfbuzz", ["-DHB_BUILD_SUBSET=OFF", "-DHB_BUILD_UTILS=OFF", "-DHB_BUILD_TESTS=OFF",
                          "-DHB_BUILD_RASTER=OFF", "-DHB_BUILD_VECTOR=OFF", "-DHB_BUILD_GPU=OFF",
                          "-DHB_HAVE_FREETYPE=ON", "-DHB_HAVE_GLIB=OFF", "-DHB_HAVE_ICU=OFF",
                          "-DHB_HAVE_GOBJECT=OFF", "-DHB_HAVE_CAIRO=OFF"]),
        ):
            folder = work / name
            arm.run(["cmake", "-S", str(trees[name]), "-B", str(folder), "-DCMAKE_BUILD_TYPE=Release",
                     f"-DCMAKE_INSTALL_PREFIX={prefix}", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                     "-DBUILD_SHARED_LIBS=OFF", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", *options], env=env)
            arm.run(["cmake", "--build", str(folder), "--parallel", "6"], env=env)
            arm.run(["cmake", "--install", str(folder)], env=env)
    args = ["cmake", "-S", str(ROOT), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release", "-DRSS_CRE=ON",
            f"-DRSS_CRE_SOURCE={trees['cre']}", "-DRSS_DESKTOP=" + ("ON" if target == "host" else "OFF"),
            "-DBUILD_TESTING=" + ("ON" if target == "host" else "OFF")]
    if toolchain:
        args.append(f"-DCMAKE_TOOLCHAIN_FILE={toolchain}")
    arm.run(args, env=env)
    arm.run(["cmake", "--build", str(build), *(["--target", "kindle-rss", "kindle-rss-home"] if target == "arm" else []),
             "--parallel", "6"], env=env)
    binary = build / "kindle-rss"
    if target == "arm":
        arm.verify_elf(binary)
    return binary


def main():
    if sys.argv[1:] == ["source-path"]:
        print(source_path("cre"))
    elif sys.argv[1:] == ["host"]:
        print(build_app("host", ROOT / "build", ROOT / "build-host-reader/prefix"))
    else:
        raise SystemExit("Usage: build_reader.py host|source-path; use build_arm.py for the complete ARM build")


if __name__ == "__main__":
    main()
