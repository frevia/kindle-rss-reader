#!/usr/bin/env python3
"""Build and verify a deterministic archive of pinned third-party source archives."""

from __future__ import annotations

import hashlib
import json
import shutil
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from urllib.parse import urlparse

import build_arm


ROOT = Path(__file__).resolve().parents[1]
LOCKS = {
    "arm": ROOT / "tools" / "arm-sources.json",
    "reader": ROOT / "tools" / "reader-sources.json",
    "toolchain": ROOT / "tools" / "toolchain-sources.json",
}
BUNDLE_NAME = "kindle-rss-third-party-sources-dev.zip"
README = (
    "Kindle RSS third-party corresponding source archives (GPLv3 distribution)\n"
    "\n"
    "This bundle contains the unmodified upstream source archives for every\n"
    "dependency in tools/arm-sources.json and tools/reader-sources.json, plus\n"
    "the pinned Zig toolchain archive in tools/toolchain-sources.json. To\n"
    "rebuild this bundle, run: python3 tools/source_bundle.py <output-directory>\n"
    "from the native-rss directory of the project source checkout; the lock files pin each source revision\n"
    "and checksum. The Zig archive includes the compiler runtime source code\n"
    "used when linking (musl, libc++, libc++abi, libunwind and compiler-rt),\n"
    "as well as the tools needed to reproduce the build.\n"
)


def _read_locks() -> dict[str, dict[str, dict[str, str]]]:
    return {group: json.loads(path.read_text()) for group, path in LOCKS.items()}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _canonical_sha256(value: object) -> str:
    raw = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(raw).hexdigest()


def _source_info(group: str, name: str, spec: dict[str, str]) -> dict[str, str]:
    item = dict(spec)
    generated_url = False
    if group == "reader":
        generated_url = not spec.get("url")
        item["url"] = spec.get("url") or f"https://codeload.github.com/{spec['repo']}/tar.gz/{spec['revision']}"
        item["sha256_type"] = spec.get("sha256_type", "archive")
    filename = PurePosixPath(urlparse(item["url"]).path).name
    # GitHub's codeload URL ends in the revision rather than a filename.
    if generated_url:
        filename += ".tar.gz"
    safe_name = "".join(c if c.isalnum() or c in "._-" else "_" for c in filename)
    return {
        "group": group,
        "name": name,
        "url": item["url"],
        "filename": filename,
        "member": f"archives/{group}-{name}-{safe_name}",
        "version": item.get("version", item.get("revision", "")),
        "revision": item.get("revision", ""),
        "repo": item.get("repo", ""),
        "declared_sha256": item["sha256"],
        "sha256_type": item.get("sha256_type", "archive"),
        "lock_sha256": _canonical_sha256(spec),
    }


def _expected_sources(locks: dict[str, dict[str, dict[str, str]]]) -> list[dict[str, str]]:
    sources = [
        _source_info(group, name, spec)
        for group in ("arm", "reader", "toolchain")
        for name, spec in sorted(locks[group].items())
    ]
    members = [source["member"] for source in sources]
    if len(members) != len(set(members)):
        raise ValueError("Source locks produce duplicate archive member names")
    return sources


def _zipinfo(name: str, *, stored: bool = False) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    info.flag_bits |= 0x800
    return info


def build_bundle(output: Path) -> Path:
    """Fetch and verify locked upstream archives, then write a reproducible ZIP."""
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    locks = _read_locks()
    sources = _expected_sources(locks)
    records = []
    with tempfile.TemporaryDirectory(prefix="kindle-rss-sources-") as temp:
        scratch = Path(temp)
        staged: dict[str, Path] = {}
        for source in sources:
            spec = dict(locks[source["group"]][source["name"]])
            spec["url"] = source["url"]
            spec["sha256_type"] = source["sha256_type"]
            # Reuse only checksum-verified downloads. Copy each immediately so
            # equal URL basenames cannot overwrite a previously staged source.
            archive = build_arm.download(source["name"], spec)
            if build_arm.source_sha256(archive, spec) != source["declared_sha256"]:
                raise ValueError(f"Source checksum mismatch: {source['group']}/{source['name']}")
            saved = scratch / (source["group"] + "-" + source["name"])
            shutil.copyfile(archive, saved)
            staged[source["member"]] = saved
            records.append({**source, "archive_sha256": _sha256(saved)})

        manifest = {"format_version": 1, "sources": records}
        target = output / BUNDLE_NAME
        temporary = target.with_suffix(".zip.tmp")
        with zipfile.ZipFile(temporary, "w", allowZip64=True) as bundle:
            bundle.writestr(_zipinfo("README.txt"), README.encode())
            bundle.writestr(_zipinfo("MANIFEST.json"),
                            (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode())
            for member in sorted(staged):
                info = _zipinfo(member, stored=True)
                with staged[member].open("rb") as source, bundle.open(info, "w") as dest:
                    shutil.copyfileobj(source, dest, 1024 * 1024)
        temporary.replace(target)
    return target


def verify_bundle(path: Path) -> dict[str, object]:
    """Verify exact lock coverage, manifest provenance and archive checksums."""
    path = Path(path)
    locks = _read_locks()
    expected = _expected_sources(locks)
    expected_members = {"README.txt", "MANIFEST.json", *(item["member"] for item in expected)}
    with zipfile.ZipFile(path) as bundle:
        infos = bundle.infolist()
        names = [item.filename for item in infos]
        if len(names) != len(set(names)):
            raise ValueError("Duplicate ZIP member names")
        if set(names) != expected_members:
            missing, extra = sorted(expected_members - set(names)), sorted(set(names) - expected_members)
            raise ValueError(f"ZIP member set mismatch (missing={missing}, extra={extra})")
        if any(name.startswith("/") or ".." in PurePosixPath(name).parts for name in names):
            raise ValueError("Unsafe ZIP member path")
        if bundle.read("README.txt") != README.encode():
            raise ValueError("README does not match this source bundle format")
        manifest = json.loads(bundle.read("MANIFEST.json"))
        if (set(manifest) != {"format_version", "sources"} or manifest.get("format_version") != 1
                or not isinstance(manifest.get("sources"), list)):
            raise ValueError("Unsupported or malformed source bundle manifest")
        expected_by_member = {item["member"]: item for item in expected}
        records = manifest["sources"]
        record_members = [record.get("member") for record in records if isinstance(record, dict)]
        if len(records) != len(expected) or set(record_members) != set(expected_by_member) or len(record_members) != len(set(record_members)):
            raise ValueError("Manifest source entries do not match current source locks")
        with tempfile.TemporaryDirectory(prefix="kindle-rss-verify-") as temp:
            scratch = Path(temp)
            for record in records:
                member = record["member"]
                source = expected_by_member[member]
                if set(record) != set(source) | {"archive_sha256"}:
                    raise ValueError(f"Malformed manifest source entry: {member}")
                if any(record.get(key) != value for key, value in source.items()):
                    raise ValueError(f"Manifest provenance does not match lock: {member}")
                extracted = scratch / Path(member).name
                with bundle.open(member) as src, extracted.open("wb") as dest:
                    shutil.copyfileobj(src, dest, 1024 * 1024)
                if _sha256(extracted) != record.get("archive_sha256"):
                    raise ValueError(f"Archive byte SHA-256 mismatch: {member}")
                spec = dict(locks[source["group"]][source["name"]])
                spec["url"] = source["url"]
                spec["sha256_type"] = source["sha256_type"]
                if build_arm.source_sha256(extracted, spec) != source["declared_sha256"]:
                    raise ValueError(f"Locked source checksum mismatch: {member}")
    return manifest


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(build_bundle(args.output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
