#!/usr/bin/env python3
"""Render version-specific release notes with working links on GitHub Releases."""
import re
import sys
from pathlib import Path


def render(tag, commit):
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag) or not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected a stable release tag and full commit SHA")
    root = Path(__file__).resolve().parents[2]
    document = root / "native-rss/docs" / f"RELEASE-{tag}.md"
    def replace(match):
        label, target = match.groups()
        if re.match(r"[a-z]+:", target) or target.startswith("#"):
            return match.group(0)
        path, _, anchor = target.partition("#")
        relative = (document.parent / path).resolve().relative_to(root)
        return f"[{label}](https://github.com/frevia/kindle-rss-reader/blob/{commit}/{relative.as_posix()}" + ("#" + anchor if anchor else "") + ")"
    return re.sub(r"\[([^\]]+)\]\(([^)]+)\)", replace, document.read_text())


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: release_notes.py vMAJOR.MINOR.PATCH COMMIT")
    print(render(*sys.argv[1:]))
