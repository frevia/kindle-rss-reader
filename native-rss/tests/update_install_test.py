"""Verify the installer screen handoff with the real coordinator and temporary files."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
from update_test import VERSION, make_zip, payload

installer, updater = map(lambda p: str(Path(p).resolve()), sys.argv[1:])
for fail in (False, True):
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        (root / 'native-rss/bin').mkdir(parents=True)
        (root / 'native-rss/bin/kindle-rss').write_text('old reader')
        archive = root / 'package.zip'
        make_zip(archive, payload())
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        subprocess.run([updater, 'stage', str(root), str(archive), VERSION, digest], check=True, timeout=15)
        if fail:
            (root / 'native-rss/.update/stage/native-rss/bin/kindle-rss').write_text('corrupt')
        subprocess.run([installer, str(root), 'fail' if fail else 'ok'], check=True, timeout=15)
