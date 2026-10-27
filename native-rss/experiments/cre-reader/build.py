#!/usr/bin/env python3
"""Build isolated host/ARM probes; reuse the project's compiler and verified downloader."""
from __future__ import annotations
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "output/cre-reader-20261027"
spec = importlib.util.spec_from_file_location("rss_arm", ROOT / "tools/build_arm.py")
arm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(arm)


def pc(prefix, name, version, library, includes=""):
    (prefix / "lib/pkgconfig").mkdir(parents=True, exist_ok=True)
    (prefix / "lib/pkgconfig" / (name + ".pc")).write_text(
        f"prefix={prefix}\nName: {name}\nDescription: isolated reading probe dependency\n"
        f"Version: {version}\nLibs: -L${{prefix}}/lib -l{library}\n"
        f"Cflags: -I${{prefix}}/include{includes}\n")


def main():
    if sys.argv[1:] not in (["host"], ["arm"], ["host", "app"], ["arm", "app"]):
        raise SystemExit("Usage: build.py host|arm [app]")
    target = sys.argv[1]
    app = len(sys.argv) == 3
    prefix = OUT / (target + "-prefix")
    arm.BUILD = OUT / (target + "-build")
    arm.PREFIX = prefix
    arm.LOGS = OUT / (target + "-logs")
    for folder in (prefix / "include", prefix / "lib", arm.BUILD, arm.LOGS):
        folder.mkdir(parents=True, exist_ok=True)
    lock = json.loads((HERE / "sources.json").read_text())
    trees = {}
    for name in (lock if target == "arm" else ("cre", "xxhash", "unibreak")):
        source = lock[name]
        cached = ROOT / "vendor" / (("crengine" if name == "cre" else name) + "-" + source["revision"] + ".tar.gz")
        arm.ARCHIVES.mkdir(parents=True, exist_ok=True)
        if cached.is_file() and arm.sha256(cached) == source["sha256"]:
            shutil.copy2(cached, arm.ARCHIVES / source["revision"])
        archive = arm.download("probe-" + name, {
            "url": f"https://codeload.github.com/{source['repo']}/tar.gz/{source['revision']}",
            "sha256": source["sha256"]})
        tree = ROOT / "vendor/reader-sources" / (name + "-" + source["revision"])
        marker = tree / ".verified-source"
        if not marker.exists() or marker.read_text() != source["sha256"]:
            arm.extract(archive, tree)
            marker.write_text(source["sha256"])
        trees[name] = tree
    env = os.environ.copy()
    toolchain = None
    if target == "arm":
        original = ROOT / "build-arm/prefix"
        if not (original / "lib/libfreetype.a").is_file():
            raise RuntimeError("Build the project's ARM dependencies first")
        # Copy into the experiment so new dependencies cannot affect release builds.
        shutil.copytree(original, prefix, dirs_exist_ok=True)
        if app:
            version = re.search(r'#define SQLITE_VERSION\s+"([^"]+)"', (prefix / "include/sqlite3.h").read_text()).group(1)
            pc(prefix, "sqlite3", version, "sqlite3")
        cc, _, toolchain = arm.write_toolchain()
        ar = [str(arm.ZIG), "ar"]
        env.update(PKG_CONFIG_PATH="", PKG_CONFIG_DIR="", PKG_CONFIG_LIBDIR=str(prefix / "lib/pkgconfig"))
    else:
        cc = "cc"
        ar = ["ar"]
        env["PKG_CONFIG_PATH"] = os.pathsep.join([str(prefix / "lib/pkgconfig"),
            str(ROOT / "build-host-media/prefix/lib/pkgconfig"), env.get("PKG_CONFIG_PATH", "")])

    def archive(name, sources, version, headers):
        build = arm.BUILD / name
        build.mkdir(exist_ok=True)
        objects = []
        for source in sources:
            obj = build / (source.stem + ".o")
            arm.run([str(cc), "-Os", "-I", str(source.parent), "-c", str(source), "-o", str(obj)],
                    log_name=name + "-" + source.stem + ".log")
            objects.append(str(obj))
        arm.run([*ar, "rcs", str(prefix / "lib" / ("lib" + name + ".a")), *objects])
        for header in headers:
            shutil.copy2(header, prefix / "include" / header.name)
        pc(prefix, "lib" + name, version, name)

    xx = trees["xxhash"]
    archive("xxhash", [xx / "xxhash.c"], "0.8.4", [xx / "xxhash.h"])
    uni = trees["unibreak"] / "src"
    units = "unibreakbase unibreakdef linebreak linebreakdata linebreakdef eastasianwidthdef emojidef graphemebreak wordbreak".split()
    archive("unibreak", [uni / (unit + ".c") for unit in units], "6.1", list(uni.glob("*.h")))
    if target == "arm":
        arm.cmake_build("png", trees["png"], toolchain,
                        ["-DPNG_SHARED=OFF", "-DPNG_STATIC=ON", "-DPNG_TESTS=OFF", "-DPNG_EXECUTABLES=OFF"], env)
        arm.cmake_build("harfbuzz", trees["harfbuzz"], toolchain,
                        ["-DHB_BUILD_SUBSET=OFF", "-DHB_BUILD_UTILS=OFF", "-DHB_BUILD_TESTS=OFF",
                         "-DHB_BUILD_RASTER=OFF", "-DHB_BUILD_VECTOR=OFF", "-DHB_BUILD_GPU=OFF",
                         "-DHB_HAVE_FREETYPE=ON", "-DHB_HAVE_GLIB=OFF", "-DHB_HAVE_ICU=OFF",
                         "-DHB_HAVE_GOBJECT=OFF", "-DHB_HAVE_CAIRO=OFF"], env)
    build = OUT / (("in-app-" if app else "") + target)
    args = ["cmake", "-S", str(ROOT if app else HERE), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
            ("-DRSS_CRE_SOURCE=" if app else "-DCRE_SOURCE=") + str(trees["cre"])]
    if app:
        args += ["-DRSS_DESKTOP=" + ("ON" if target == "host" else "OFF"),
                 "-DBUILD_TESTING=" + ("ON" if target == "host" else "OFF")]
    if toolchain:
        args += ["-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain)]
    arm.run(args, env=env, log_name="probe-configure.log")
    targets = (["--target", "kindle-rss"] if target == "arm" else []) if app else ["--target", "cre-probe", "baseline", "page-preview"]
    arm.run(["cmake", "--build", str(build), *targets, "--parallel", "6"],
            env=env, log_name="probe-build.log")
    if target == "arm":
        for name in (("kindle-rss",) if app else ("cre-probe", "baseline", "page-preview")):
            arm.verify_elf(build / name)
    print("Probes ready:", build, flush=True)


if __name__ == "__main__":
    main()
