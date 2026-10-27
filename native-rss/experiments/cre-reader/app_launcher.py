"""Generate an isolated real-app launcher; keep the production lock and database."""
import re
import sys
from pathlib import Path


def launcher(folder):
    if not re.fullmatch(r"/mnt/us/rss-backups/rss-cre-app\.[A-Za-z0-9]+", folder):
        raise ValueError("expected a private rss-cre-app test directory")
    source = (Path(__file__).resolve().parents[2] / "device/launch.sh").read_text()
    changes = {
        "APP=/mnt/us/native-rss/bin/kindle-rss\n": f"APP={folder}/kindle-rss-cre\n",
        "snapshot=\n": "snapshot=\nCRE_CACHE=\n",
        "    stop_child\n    restore_failed=0\n":
            '    stop_child\n    [ -z "$CRE_CACHE" ] || rm -rf "$CRE_CACHE"\n    restore_failed=0\n',
        "done_file=$snapshot.done\n":
            'CRE_CACHE=$(mktemp -d /tmp/rss-cre-session.XXXXXX) || fail \'无法创建新版排版缓存\'\n'
            'export RSS_CRE_CACHE=$CRE_CACHE\ndone_file=$snapshot.done\n',
    }
    for old, new in changes.items():
        if source.count(old) != 1:
            raise ValueError("launcher changed; review lifecycle integration before deployment")
        source = source.replace(old, new)
    assert "STATE=/var/local/native-rss\n" in source
    assert "DB=/mnt/us/native-rss/data/rss.sqlite\n" in source
    return source


if __name__ == "__main__":
    print(launcher(sys.argv[1]), end="")
