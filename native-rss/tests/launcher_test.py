"""Exercise the actual launcher with fake device commands, never host system services."""
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "device/launch.sh"
FAKE = r'''
import fcntl, os, pathlib, signal, subprocess, sys, time
root=pathlib.Path(os.environ["FIXTURE"])
name=pathlib.Path(sys.argv[0]).name
args=sys.argv[1:]
mode=os.environ["SCENARIO"]
def log(text):
    with (root/"events").open("a") as f:f.write(text+"\n")
if name=="stat":
    print("2700:0" if mode=="inherited-setgid" else "700:0")
elif name=="flock":
    try:fcntl.flock(9,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except OSError:sys.exit(1)
elif name=="timeout":
    try:sys.exit(subprocess.run(args[1:],timeout=float(args[0])).returncode)
    except subprocess.TimeoutExpired:sys.exit(124)
elif name=="pidof":
    print({"awesome":900001,"JunoStatusBarDriver":900002,"volumd":900003}[args[0]])
elif name=="get":
    if mode=="power-error" and (root/"started").exists():sys.exit(1)
    print("screenSaver" if mode=="sleep" and (root/"started").exists() else "active")
elif name=="set":
    log("set "+" ".join(args))
    if mode=="pillow-error" and args[-1]=="disable":sys.exit(1)
elif name=="refresh":log("refresh")
elif name=="kill":
    sig,pid=args[0],int(args[1])
    if pid>=900000:
        log(sig+" "+str(pid))
        if mode=="launcher-killed-early" and sig=="-STOP" and pid==900001:
            os.kill(os.getppid(),signal.SIGKILL)
        if mode=="stop-error" and sig=="-STOP" and pid==900002:sys.exit(1)
    else:
        try:os.kill(pid,0 if sig=="-0" else getattr(signal,"SIG"+sig[1:]))
        except ProcessLookupError:sys.exit(1)
elif name=="app":
    if args==["probe"]:
        print("width=1072\nheight=1448\nstride=1088\nbpp=8\nxoffset=0\nyoffset=0\nrotation=3\ndriver=mxc_epdc_fb\nlayout=accepted")
    else:
        assert os.environ["RSS_UI_GUARD"]=="1"
        (root/"started").touch()
        with (root/"fb").open("r+b") as f:f.write(b"!")
        log("app-start")
        if mode=="launcher-killed":
            owner=int(os.environ["RSS_LAUNCHER_PID"])
            while os.getppid()==owner:time.sleep(.02)
            time.sleep(2)
            log("app-terminated")
            sys.exit(0)
        if mode in ("sleep","power-error"):time.sleep(20)
        if mode=="crash":sys.exit(7)
        if mode=="pid-reuse":
            p=root/"proc/900001/stat"
            p.write_text(p.read_text().replace(" 123\n"," 999\n"))
else:raise RuntimeError(name)
'''


class LauncherTests(unittest.TestCase):
    def run_case(self, mode):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/"bin").mkdir();(root/"state").mkdir()
            (root/"state/session.log").write_text("previous session fixture\n")
            if mode=="log-symlink":
                (root/"protected").write_text("untouched")
                (root/"state/session.log").unlink()
                (root/"state/session.log").symlink_to(root/"protected")
            for name in ("stat","flock","timeout","pidof","get","set","refresh","kill","app"):
                tool=root/"bin"/name;tool.write_text("#!"+sys.executable+"\n"+FAKE);tool.chmod(0o755)
            for pid,name in ((900001,"awesome"),(900002,"JunoStatusBarDr"),(900003,"volumd")):
                p=root/"proc"/str(pid);p.mkdir(parents=True)
                (p/"comm").write_text(name+"\n")
                (p/"stat").write_text(" ".join([str(pid),"("+name+")","S"]+["0"]*18+["123"])+"\n")
                (p/"status").write_text("State:\tS (sleeping)\n")
            (root/"fb").write_bytes(b"x"*1575424)
            if mode=="passcode":(root/"passcode").touch()
            values={"STATE":root/"state","APP":root/"bin/app","DB":root/"rss.sqlite","FB":root/"fb",
                    "PROC":root/"proc","GET":root/"bin/get","SET":root/"bin/set","REFRESH":root/"bin/refresh","KILL":root/"bin/kill","PASSCODE":root/"passcode"}
            script=SOURCE.read_text().splitlines()
            for i,line in enumerate(script):
                key=line.split("=",1)[0]
                if key in values:script[i]=key+"='"+str(values[key])+"'"
            source="\n".join(script)+"\n"
            # macOS has no /proc; the production guardian uses Linux start time and zombie state.
            source=re.sub(r'^owner_born=.*$', 'owner_born=123',source,flags=re.M)
            source=re.sub(r'owner_alive\(\) \{.*?\n\}', 'owner_alive() { "$KILL" -0 "$owner" 2>/dev/null; }',source,flags=re.S)
            launch=root/"launch.sh";launch.write_text(source)
            env=dict(os.environ,FIXTURE=folder,SCENARIO=mode,PATH=str(root/"bin")+os.pathsep+os.environ["PATH"])
            if mode.startswith("launcher-killed"):
                process=subprocess.Popen(["/bin/sh",str(launch)],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                try:
                    if mode=="launcher-killed":
                        end=time.monotonic()+10
                        while not (root/"started").exists() and time.monotonic()<end:time.sleep(.02)
                        self.assertTrue((root/"started").exists())
                        process.kill()
                    process.wait(timeout=10)
                    out,err=process.communicate(timeout=10)
                    result=subprocess.CompletedProcess(process.args,process.returncode,out,err)
                    end=time.monotonic()+10
                    while time.monotonic()<end:
                        log=(root/"state/session.log").read_text()
                        if "session_end" in log and not list((root/"state").glob("frame.*")):break
                        time.sleep(.02)
                finally:
                    if process.poll() is None:process.kill();process.wait()
            else:
                result=subprocess.run(["/bin/sh",str(launch)],env=env,capture_output=True,text=True,timeout=15)
            events=(root/"events").read_text().splitlines() if (root/"events").exists() else []
            self.assertFalse(list((root/"state").glob("frame.*")))
            if mode not in ("passcode","log-symlink"):
                self.assertEqual((root/"state/session.previous.log").read_text(),"previous session fixture\n")
                log=(root/"state/session.log").read_text()
                self.assertIn("session_start",log);self.assertIn("session_end",log)
            if mode=="log-symlink":self.assertEqual((root/"protected").read_text(),"untouched")
            return result,events,(root/"fb").read_bytes()[0:1]

    def test_normal_and_crash_restore(self):
        for mode,code in (("normal",0),("inherited-setgid",0),("crash",7)):
            with self.subTest(mode=mode):
                result,events,pixel=self.run_case(mode)
                self.assertEqual(result.returncode,code,result.stderr)
                self.assertEqual(pixel,b"x")
                for pid in (900001,900002,900003):self.assertIn("-CONT "+str(pid),events)
                self.assertLess(events.index("refresh"),events.index("-CONT 900003"))

    def test_sleep_and_unknown_power_do_not_overwrite_screen(self):
        for mode in ("sleep","power-error"):
            with self.subTest(mode=mode):
                result,events,pixel=self.run_case(mode)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(pixel,b"!");self.assertNotIn("refresh",events)
                self.assertIn("-CONT 900003",events)

    def test_partial_takeover_failure_resumes_touched_services(self):
        result,events,pixel=self.run_case("stop-error")
        self.assertEqual(result.returncode,1)
        self.assertNotIn("app-start",events)
        self.assertIn("-CONT 900001",events)
        self.assertIn("set com.lab126.pillow disableEnablePillow enable",events)

    def test_pillow_failure_still_restores_pillow(self):
        result,events,pixel=self.run_case("pillow-error")
        self.assertEqual(result.returncode,1)
        self.assertNotIn("-STOP 900001",events)
        self.assertIn("set com.lab126.pillow disableEnablePillow enable",events)

    def test_reused_pid_is_not_signalled(self):
        result,events,pixel=self.run_case("pid-reuse")
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertNotIn("-CONT 900001",events)
        self.assertEqual(pixel,b"!");self.assertNotIn("refresh",events)

    def test_passcode_prevents_all_takeover_actions(self):
        result,events,pixel=self.run_case("passcode")
        self.assertEqual(result.returncode,1)
        self.assertEqual(events,[]);self.assertEqual(pixel,b"x")

    def test_log_symlink_is_rejected_before_takeover(self):
        result,events,pixel=self.run_case("log-symlink")
        self.assertEqual(result.returncode,1)
        self.assertEqual(events,[]);self.assertEqual(pixel,b"x")

    def test_diagnostic_exports_only_recent_log_lines(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);state=root/"state";state.mkdir()
            (state/"session.log").write_text("".join(f"fixture-line-{i:03d}\n" for i in range(100)))
            (root/"private").write_text("must-not-export-this")
            (state/"session.previous.log").symlink_to(root/"private")
            source=(SOURCE.parent/"diagnose.sh").read_text()
            source=source.replace("/var/local/native-rss",str(state))
            source=source.replace("/mnt/us/documents/RSS-device-report.txt",str(root/"report"))
            # Skip hardware commands; this test exercises the real report/log export loop.
            source=source.replace("if timeout 2 /bin/true 2>/dev/null; then","if false; then")
            script=root/"diagnose.sh";script.write_text(source)
            subprocess.run(["/bin/sh",str(script)],check=True,capture_output=True,timeout=10)
            report=(root/"report").read_text()
            self.assertNotIn("fixture-line-035",report)
            self.assertIn("fixture-line-036",report);self.assertIn("fixture-line-099",report)
            self.assertNotIn("must-not-export-this",report)

    def test_killed_launcher_is_recovered_by_guardian(self):
        for mode in ("launcher-killed", "launcher-killed-early"):
            with self.subTest(mode=mode):
                result,events,pixel=self.run_case(mode)
                self.assertEqual(result.returncode,-9,result.stderr)
                self.assertEqual(pixel,b"x")
                for pid in (900001,900002,900003):self.assertIn("-CONT "+str(pid),events)
                self.assertLess(events.index("refresh"),events.index("-CONT 900003"))
                if mode=="launcher-killed":
                    self.assertLess(events.index("app-terminated"),events.index("-CONT 900001"))


if __name__=="__main__":unittest.main()
