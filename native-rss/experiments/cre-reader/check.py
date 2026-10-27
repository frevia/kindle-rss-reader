#!/usr/bin/env python3
"""Render the same four fixtures and verify output, anchors and input rejection."""
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "output/cre-reader-20261027"


def main():
    if len(sys.argv) != 1:
        raise SystemExit("Usage: check.py (runs the host probes)")
    fixtures = OUT / "fixtures"
    subprocess.run([sys.executable, str(HERE / "make_fixtures.py"), str(fixtures)], check=True)
    font = ROOT / "vendor/runtime/reader.ttf"
    revision = json.loads((HERE / "sources.json").read_text())["cre"]["revision"]
    css = ROOT / "vendor/reader-sources" / ("cre-" + revision) / "cr3gui/data/epub.css"
    results = []
    for case in ("chinese", "mixed", "code", "images"):
        for engine, binary in (("baseline", "baseline"), ("cre", "cre-probe")):
            output = OUT / "host-results" / case / engine
            output.mkdir(parents=True, exist_ok=True)
            for page in output.glob("page-*.pgm"):
                page.unlink()
            args = [str(OUT / "host" / binary), str(font), str(fixtures / (case + ".html"))]
            if engine == "cre":
                args.append(str(css))
            process = subprocess.run([*args, str(output)], capture_output=True, text=True, timeout=120)
            (output / "process.log").write_text(process.stdout + process.stderr)
            if process.returncode:
                raise RuntimeError(f"{case}/{engine} failed: {process.stderr}\n{process.stdout}")
            lines = [line for line in process.stdout.splitlines() if line.startswith('{"engine":')]
            assert len(lines) == 1
            metrics = json.loads(lines[0])
            assert metrics["pages"] > 1 and metrics["peak_rss_kib"] > 0
            assert metrics["layout_ms"] >= 0 and metrics["render_total_ms"] >= 0
            pages = list(output.glob("page-*.pgm"))
            assert len(pages) == metrics["pages"]
            header = b"P5\n1072 1448\n255\n"
            for page in pages:
                raw = page.read_bytes()
                assert raw.startswith(header) and len(raw) == len(header) + 1072 * 1448
            if engine == "cre":
                assert all(metrics[key] for key in ("anchor_reopen", "anchor_reflow", "hit_test"))
                assert metrics["image_count"] == (2 if case == "images" else 0)
                assert (output / "anchor.txt").read_text()
            metrics["case"] = case
            metrics["render_mean_ms"] = round(metrics["render_total_ms"] / metrics["pages"], 3)
            results.append(metrics)
            print(case, engine, "pages", metrics["pages"], "layout_ms", metrics["layout_ms"],
                  "render_mean_ms", metrics["render_mean_ms"], "peak_rss_kib", metrics["peak_rss_kib"], flush=True)
    invalid = OUT / "invalid-inputs"
    invalid.mkdir(exist_ok=True)
    (invalid / "empty.html").write_bytes(b"")
    (invalid / "oversized.html").write_bytes(b"x" * (8 * 1024 * 1024 + 1))
    for binary in ("baseline", "cre-probe"):
        for name in ("missing.html", "empty.html", "oversized.html"):
            args = [str(OUT / "host" / binary), str(font), str(invalid / name)]
            if binary == "cre-probe":
                args.append(str(css))
            bad = subprocess.run([*args, str(invalid / "output")], capture_output=True, timeout=15)
            assert bad.returncode != 0, (binary, name)
    (OUT / "host-metrics.json").write_text(json.dumps(results, indent=2) + "\n")
    preview = subprocess.run([str(OUT / "host/page-preview")], capture_output=True, timeout=10)
    assert preview.returncode != 0 and b"screen-session launcher" in preview.stderr
    print("8 rendering runs, 6 invalid-input checks and preview launcher-guard check passed")


if __name__ == "__main__":
    main()
