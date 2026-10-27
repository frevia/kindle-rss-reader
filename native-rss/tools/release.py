#!/usr/bin/env python3
"""Stage the two development ZIPs as versioned release attachments."""
import re
import json
import shutil
import sys
from pathlib import Path
import zipfile

import package


def stage_release(output: Path, tag: str) -> Path:
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ValueError("tag must be a stable version such as v0.0.2")

    files = (
        ("kindle-rss-kpw3-dev.zip", f"RSS-Reader-{tag}-KPW3.zip"),
        ("kindle-rss-source-dev.zip", f"RSS-Reader-{tag}-source.zip"),
    )
    sources = [(output / source, target) for source, target in files]
    for source, _ in sources:
        if not source.is_file():
            raise FileNotFoundError(f"Missing release input: {source}")
    with zipfile.ZipFile(sources[0][0]) as archive:
        manifest = json.loads(archive.read("native-rss/MANIFEST.json"))
    if manifest.get("public_release_ready") is not True:
        raise ValueError("当前为本地 CREngine 集成包，GPLv2/Apache-2.0 许可兼容尚未确认，不能进入公开 Release")

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
    package.main()
    print(stage_release(Path(package.ROOT) / "output", tag))


if __name__ == "__main__":
    main()
