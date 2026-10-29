import hashlib
import importlib.util
from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import package as pack
from build_arm_test import elf


class PackageTests(unittest.TestCase):
    def test_sources_include_project_license_but_exclude_private_artifacts(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)/"native-rss";root.mkdir()
            public=("LICENSE","README.md","CONTRIBUTING.md","SECURITY.md","THIRD_PARTY_NOTICES.md","CHANGELOG.md",".github/workflows/release.yml")
            for name in public:
                path=root.parent/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(name)
            included=("src/main.cpp","tests/fixtures/colors.png","docs/INSTALL.md","device/INSTALL.txt","device/launch.sh",
                      "tools/reader.cmake","tools/reader-sources.json","third_party/crengine/LICENSE",
                      "home/card.js","home/firmwares.json","third_party/hbc84/LICENSE",
                      "third_party/hbc84/SOURCE.json","third_party/hbc84/opcode.json","third_party/hbc84/structure.json",
                      "experiments/cre-reader/CMakeLists.txt","experiments/cre-reader/engine.cmake",
                      "experiments/cre-reader/build.py","experiments/cre-reader/probe.cpp","experiments/cre-reader/README.txt")
            private=("docs/device-report.txt","docs/session.log","tests/private.sqlite","data/feeds.opml","output/app.zip","reader-deps/unibreak/linebreak.o","tools/__pycache__/package.pyc",
                     "experiments/cre-reader/device-report.txt","experiments/cre-reader/results.json",
                     "experiments/cre-reader/__pycache__/build.pyc","home/original.hbc","home/patched.hbc")
            for name in (*included,*private):
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b"test")
            files=pack.source_files(root)
            self.assertEqual(set(files),set(public)|{"native-rss/"+name for name in included})
            self.assertTrue(files["native-rss/device/launch.sh"][1])
            (root/"src/link.cpp").symlink_to(root/"src/main.cpp")
            with self.assertRaises(RuntimeError):pack.source_files(root)

    def test_home_adapter_files_validates_recipe_and_excludes_firmware_images(self):
        profiles=json.loads((pack.ROOT/"home/firmwares.json").read_text())["profiles"]
        profile=profiles[0]
        patched=b"patched home adapter bytes"
        recipe=(b"RSSHBC1\n"+bytes.fromhex(profile["original_sha256"])+hashlib.sha256(patched).digest()+
                struct.pack("<III",1,len(patched),1)+b"\1"+struct.pack("<I",len(patched))+patched)
        manifest=dict(profile,format=1,patch_sha256=hashlib.sha256(recipe).hexdigest(),
                      patched_sha256=hashlib.sha256(patched).hexdigest(),
                      source_sha256=pack.sha256(pack.ROOT/"home/card.js"),
                      modified_original_functions=[profile["render_function"],profile["press_function"]],
                      contains_original_firmware=False,enabled_by_default=False)
        with tempfile.TemporaryDirectory() as folder:
            adapter=Path(folder)
            (adapter/"patch.rhp").write_bytes(recipe)
            (adapter/"patched.hbc").write_bytes(patched)
            (adapter/"original.hbc").write_bytes(b"private original image")
            (adapter/"manifest.json").write_text(json.dumps(manifest))
            files=pack.home_adapter_files(adapter)
            self.assertEqual(set(files),{"native-rss/home/patch.rhp","native-rss/home/profile.env","native-rss/home/manifest.json"})
            self.assertEqual(files["native-rss/home/patch.rhp"][0],recipe)
            self.assertNotIn(patched, [data for data,_ in files.values()])
            self.assertNotIn(b"private original image", [data for data,_ in files.values()])
            for name,data in (("native-rss/home/original.hbc",b"private original image"),
                              ("native-rss/home/patched.hbc",patched)):
                with self.subTest(name=name):
                    self.assertNotIn(name,files)
            manifest["source_sha256"]="0"*64
            (adapter/"manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(RuntimeError):pack.home_adapter_files(adapter)
            manifest["source_sha256"]=pack.sha256(pack.ROOT/"home/card.js")
            (adapter/"manifest.json").write_text(json.dumps(manifest))
            (adapter/"patch.rhp").write_bytes(recipe+b"tampered")
            with self.assertRaises(RuntimeError):pack.home_adapter_files(adapter)

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
