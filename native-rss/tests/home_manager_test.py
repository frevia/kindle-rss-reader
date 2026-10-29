"""Exercise home.sh with an isolated filesystem and stubbed device commands."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "device/home.sh"
ORIGINAL = "999d9234f5557820ce13526fa7732e2bebe273a84f5a375c97f9a1936d4cafa2"
PATCHED = "a" * 64
PATCH = "b" * 64

STUB = r'''#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys, time
root=pathlib.Path(os.environ["FIXTURE"]); mode=os.environ["SCENARIO"]; name=pathlib.Path(sys.argv[0]).name; args=sys.argv[1:]
ORIGINAL=os.environ["ORIGINAL_HASH"]; PATCHED=os.environ["PATCHED_HASH"]; PATCH=os.environ["PATCH_HASH"]
def log(text):
    with (root/"events").open("a") as f: f.write(text+"\n")
def mounted(): return (root/"mounted").exists()
def do_mount():
    source,target=args[-2:]; log("mount "+target); (root/"mounted").write_text(source)
    mounts=root/"proc/mounts"; lines=[x for x in mounts.read_text().splitlines() if x.split()[1]!=target]
    lines.append(f"{source} {target} none rw,bind 0 0"); mounts.write_text("\n".join(lines)+"\n")
def do_unmount(target):
    log("umount "+target)
    if mounted(): (root/"mounted").unlink()
    mounts=root/"proc/mounts"; lines=[x for x in mounts.read_text().splitlines() if x.split()[1]!=target]
    mounts.write_text("\n".join(lines)+("\n" if lines else ""))
if name=="stat": print("2943085" if args[:2]==["-c", "%s"] else "700:0")
elif name=="df": print("Filesystem 1024-blocks Used Available Capacity Mounted on\nfixture 10000 0 "+("128" if mode=="low-space" else "10000")+" 0% /fixture")
elif name=="sha256sum":
    path=pathlib.Path(args[-1]); text=str(path); value="c"*64
    if text.endswith("patch.rhp"): value=PATCH
    elif text.endswith("original.hbc"): value=ORIGINAL
    elif text.endswith("patched.hbc"): value=PATCHED
    elif text.endswith("KPPMainApp.js.hbc"):
        value=("d"*64 if mode=="unknown-bind" and mounted() else PATCHED if mounted() else "e"*64 if mode=="target-hash-bad" else ORIGINAL)
    if mode=="patch-hash-bad" and text.endswith("patch.rhp"): value="c"*64
    print(value+"  "+text)
elif name=="nohup": log("guard-armed")
elif name=="cp":
    if mode=="job-copy-fail" and args[-1].endswith(".rss-tmp"):
        pathlib.Path(args[-1]).write_bytes(b""); sys.exit(1)
    sys.exit(subprocess.run(["/bin/cp", *args]).returncode)
elif name=="get": print("active")
elif name=="set": log("set "+" ".join(args))
elif name=="mntroot":
    log("mntroot "+args[0]); mounts=root/"proc/mounts"; rows=[]
    for line in mounts.read_text().splitlines():
        parts=line.split()
        if len(parts)>3 and parts[0]=="/dev/root" and parts[1]=="/": parts[3]="rw,relatime" if args[0]=="rw" else "ro,relatime"
        rows.append(" ".join(parts))
    mounts.write_text("\n".join(rows)+"\n")
elif name=="mount": do_mount()
elif name=="umount":
    if mode=="busy-bind" and "-l" not in args: sys.exit(1)
    do_unmount(args[-1])
elif name=="timeout":
    try: sys.exit(subprocess.run(args[1:], timeout=float(args[0])).returncode)
    except subprocess.TimeoutExpired: sys.exit(124)
elif name=="sleep": time.sleep(.01 if args[0]=="1" else 2)
elif name=="flock": sys.exit(2 if any(a.startswith("-") and a not in ("-n", "-u", "-s", "-x") for a in args) else 0)
elif name=="initctl":
    log("init "+" ".join(args))
    if args[0]=="status" and args[1]=="kppmainapp": print("kppmainapp start/running, process 321")
    elif args[0]=="status" and args[1]=="native-rss-home": print("native-rss-home start/running, process 900004")
    elif args[0]=="restart" and args[1]=="kppmainapp":
        job=pathlib.Path(os.environ["JOB"])
        if job.exists():
            result=subprocess.run(["/bin/sh",os.environ["SELF"],"mount"],env=os.environ.copy())
            if result.returncode: sys.exit(result.returncode)
            pid="900004"; comm="bad-process" if mode=="health-wrong-comm" else "kindle-rss-home"
            proc=root/"proc"/pid; proc.mkdir(exist_ok=True); (proc/"comm").write_text(comm+"\n")
            if mode in ("ready", "health-pid-mismatch", "health-wrong-comm"):
                health_pid="900005" if mode=="health-pid-mismatch" else pid
                (pathlib.Path(os.environ["STATE"])/"health").write_text(f"pid={health_pid}\nready=1\nclients=1\nrevision={'f'*64}\nacks=1\n")
    elif args[0]=="stop" and mounted(): subprocess.run(["/bin/sh",os.environ["SELF"],"unmount"],env=os.environ.copy())
elif name=="homebin":
    if args and args[0]=="patch":
        if mode=="patch-fail": sys.exit(1)
        pathlib.Path(args[3]).write_bytes(b"patched adapter")
elif name=="kill": log("kill "+" ".join(args))
else: sys.exit("unexpected stub: "+name)
'''


class HomeManagerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.app = self.root / "mnt/us/native-rss"
        self.state = self.root / "var/local/native-rss/home"
        self.native = self.root / "var/local/native-rss"
        self.proc = self.root / "proc"
        self.job = self.root / "etc/upstart/native-rss-home.conf"
        self.target = self.root / "app/KPPMainApp/js/KPPMainApp.js.hbc"
        self.bin = self.root / "bin"
        self.app.joinpath("home").mkdir(parents=True)
        self.app.joinpath("bin").mkdir()
        self.state.mkdir(parents=True)
        self.target.parent.mkdir(parents=True)
        self.job.parent.mkdir(parents=True)
        self.target.write_bytes(b"original adapter fixture")
        self.proc.mkdir()
        self.proc.joinpath("mounts").write_text("/dev/root / ext4 ro,relatime 0 0\n")
        self.app.joinpath("home/patch.rhp").write_bytes(b"recipe fixture")
        self.app.joinpath("home/native-rss-home.conf").write_text("# RSS Reader optional home card v1\n")
        self.app.joinpath("home/profile.env").write_text(
            f"ORIGINAL_SHA256={ORIGINAL}\nPATCHED_SHA256={PATCHED}\nPATCH_SHA256={PATCH}\n")
        reader_bin = self.app / "bin/kindle-rss"
        reader_bin.write_text("stub\n"); reader_bin.chmod(0o755)
        reader = self.root / "mnt/us/documents/RSS阅读器.sh"
        reader.parent.mkdir(parents=True); reader.write_text("stub\n")
        self.bin.mkdir()
        for name in ("stat", "sha256sum", "get", "set", "mntroot", "mount", "umount", "timeout", "sleep", "flock", "initctl", "nohup", "cp", "df"):
            path = self.bin / name; path.write_text(STUB.replace("#!/usr/bin/env python3", "#!" + sys.executable)); path.chmod(0o755)
        homebin = self.app / "bin/kindle-rss-home"
        homebin.write_text(STUB.replace("#!/usr/bin/env python3", "#!" + sys.executable).replace('name=pathlib.Path(sys.argv[0]).name', 'name="homebin"'))
        homebin.chmod(0o755)
        self.self_script = self.root / "mnt/us/native-rss/home.sh"
        self.self_script.parent.mkdir(parents=True, exist_ok=True)
        source = SOURCE.read_text().replace('while [ "$n" -lt 60 ]', 'while [ "$n" -lt 3 ]')
        replacements = {
            "/var/local/native-rss": str(self.native),
            "/mnt/us/native-rss": str(self.app),
            "/app/KPPMainApp/js/KPPMainApp.js.hbc": str(self.target),
            "/etc/upstart/native-rss-home.conf": str(self.job),
            "/sbin/initctl": str(self.bin / "initctl"),
            "/usr/sbin/mntroot": str(self.bin / "mntroot"),
            "/usr/bin/lipc-get-prop": str(self.bin / "get"),
            "/usr/bin/lipc-set-prop": str(self.bin / "set"),
            "/proc": str(self.proc),
            "/mnt/us/documents/RSS阅读器.sh": str(reader),
            "/var/local/system/userpasswdenabled": str(self.root / "var/local/system/userpasswdenabled"),
            "/mnt/us/documents/KPPMainAppV2_": str(self.root / "mnt/us/documents/KPPMainAppV2_"),
        }
        for old, new in replacements.items(): source = source.replace(old, new)
        source = source.replace("SELF=" + str(self.app) + "/home.sh", "SELF=" + str(self.self_script))
        source = source.replace("/usr/bin/lipc-get-prop", str(self.bin / "get"))
        self.self_script.write_text(source); self.self_script.chmod(0o755)
        self.env = dict(os.environ, FIXTURE=str(self.root), PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        SCENARIO="ready", STATE=str(self.state), JOB=str(self.job), SELF=str(self.self_script),
                        ORIGINAL_HASH=ORIGINAL, PATCHED_HASH=PATCHED, PATCH_HASH=PATCH)

    def tearDown(self): self.temp.cleanup()

    def invoke(self, command, scenario="ready", extra_env=None, arguments=()):
        env = dict(self.env, SCENARIO=scenario)
        if extra_env: env.update(extra_env)
        return subprocess.run(["/bin/sh", str(self.self_script), command, *arguments], env=env, capture_output=True, text=True, timeout=60)

    def events(self): return (self.root / "events").read_text().splitlines() if (self.root / "events").exists() else []

    def test_unsupported_firmware_and_integrity_fail_without_system_changes(self):
        profile = self.app / "home/profile.env"
        profile.write_text(f"ORIGINAL_SHA256={'0'*64}\nPATCHED_SHA256={PATCHED}\nPATCH_SHA256={PATCH}\n")
        result = self.invoke("enable")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.job.exists()); self.assertFalse((self.root / "mounted").exists())
        self.assertFalse((self.state / "original.hbc").exists())
        self.assertEqual(self.events(), [])
        profile.write_text(f"ORIGINAL_SHA256={ORIGINAL}\nPATCHED_SHA256={PATCHED}\nPATCH_SHA256={PATCH}\n")
        result = self.invoke("enable", "patch-hash-bad")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.job.exists()); self.assertFalse((self.root / "mounted").exists())
        self.assertEqual(self.events(), [])

    def test_low_state_space_refuses_before_copy_or_system_mutation(self):
        result = self.invoke("enable", "low-space")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.state / "original.hbc").exists())
        self.assertFalse(self.job.exists())
        self.assertFalse((self.root / "mounted").exists())
        self.assertEqual(self.events(), [])

    def test_restart_reports_stay_on_usb_and_only_match_restarted_pid(self):
        documents = self.root / "mnt/us/documents"
        report = documents / "KPPMainAppV2_321_fixture.txt"
        other = documents / "KPPMainAppV2_999_fixture.txt"
        report.write_text("own restart report")
        other.write_text("unrelated report")
        result = self.invoke("enable")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(report.exists())
        self.assertEqual((self.root / "mnt/us/native-rss-backups/home-restart-reports" / report.name).read_text(), "own restart report")
        self.assertTrue(other.exists())
        self.assertFalse((self.state / "restart-reports").exists())

    def test_late_restart_report_is_archived_without_touching_the_active_dialog(self):
        self.assertEqual(self.invoke("enable").returncode, 0)
        report = self.root / "mnt/us/documents/KPPMainAppV2_321_late.txt"
        report.write_text("finished after startup")
        before = self.events()
        result = self.invoke("archive-after", arguments=("321",))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(report.exists())
        self.assertEqual(self.events(), before)

    def test_enable_waits_for_matching_ack_and_restores_root_readonly(self):
        result = self.invoke("enable")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / "mounted").exists())
        self.assertTrue((self.state / "enabled").exists())
        self.assertIn("ready=1", (self.state / "health").read_text())
        self.assertEqual(self.job.read_text().splitlines()[0], "# RSS Reader optional home card v1")
        self.assertEqual(self.events().count("mntroot rw"), 1)
        self.assertEqual(self.events().count("mntroot ro"), 1)
        self.assertFalse((self.state / "root-was-ro").exists())
        self.assertEqual(self.target.read_bytes(), b"original adapter fixture")

    def test_stale_or_forged_ack_automatically_restores_original(self):
        for scenario in ("health-pid-mismatch", "health-wrong-comm", "health-never-ready"):
            with self.subTest(scenario=scenario):
                result = self.invoke("enable", scenario)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.job.exists())
                self.assertFalse((self.root / "mounted").exists())
                self.assertFalse((self.state / "enabled").exists())
                self.assertFalse((self.state / "root-was-ro").exists())
                events = self.events()
                self.assertIn("umount " + str(self.target), events)
                self.assertIn("mntroot rw", events); self.assertIn("mntroot ro", events)

    def test_partial_configuration_copy_is_removed_and_can_retry(self):
        result = self.invoke("enable", "job-copy-fail")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.job.exists())
        self.assertFalse(Path(str(self.job) + ".rss-tmp").exists())
        self.assertFalse((self.state / "installing-job").exists())
        self.assertFalse((self.state / "root-was-ro").exists())
        self.assertIn("mntroot ro", self.events())
        result = self.invoke("enable")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_independent_guard_recovers_config_mount_and_readonly_marker(self):
        result = self.invoke("enable")
        self.assertEqual(result.returncode, 0, result.stderr)
        pending = self.state / "activate.interrupted"
        pending.touch()
        (self.state / "root-was-ro").touch()
        mounts = self.proc / "mounts"
        mounts.write_text(mounts.read_text().replace("ro,relatime", "rw,relatime"))
        result = self.invoke("rollback-after", arguments=(str(pending),))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.job.exists())
        self.assertFalse((self.root / "mounted").exists())
        self.assertFalse((self.state / "enabled").exists())
        self.assertFalse((self.state / "root-was-ro").exists())
        self.assertFalse(pending.exists())
        self.assertIn("/dev/root / ext4 ro,relatime", mounts.read_text())

    def test_unknown_startup_symlink_is_preserved(self):
        destination = self.root / "missing-config"
        self.job.symlink_to(destination)
        for command in ("enable", "disable"):
            result = self.invoke(command)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(self.job.is_symlink())
        self.assertFalse(destination.exists())
        self.assertFalse(any(x.startswith("mntroot ") for x in self.events()))

    def test_disable_detaches_owned_bind_while_home_still_maps_it(self):
        self.assertEqual(self.invoke("enable").returncode, 0)
        result = self.invoke("disable", "busy-bind")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root / "mounted").exists())
        self.assertFalse(self.job.exists())
        self.assertEqual(self.target.read_bytes(), b"original adapter fixture")

    def test_disable_is_idempotent_and_unknown_bind_is_never_unmounted(self):
        first = self.invoke("disable"); second = self.invoke("disable")
        self.assertEqual(first.returncode, 0, first.stderr); self.assertEqual(second.returncode, 0, second.stderr)
        self.assertFalse(any(x.startswith("umount ") for x in self.events()))
        self.assertFalse(any(x.startswith("init restart ") for x in self.events()))
        mounts = self.proc / "mounts"
        mounts.write_text(mounts.read_text() + f"unknown-source {self.target} none rw,bind 0 0\n")
        (self.state / "mounted.sha256").write_text(PATCHED + "\n")
        (self.root / "mounted").write_text("unknown-source")
        result = self.invoke("disable", "unknown-bind")
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.root / "mounted").exists())
        self.assertFalse(any(x.startswith("umount ") for x in self.events()))


if __name__ == "__main__": unittest.main()
