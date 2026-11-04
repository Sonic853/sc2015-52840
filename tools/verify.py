"""Check saved build/test provenance and source identity without rebuilding."""

import ast
import argparse
import hashlib
import json
from pathlib import Path
import re

from image import validate_uf2
from targets import TARGETS, uf2_name

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=TARGETS, default="xiao")
    args = parser.parse_args()
    manifest = json.loads((ROOT / "build" / args.target / "manifest.json").read_text())
    tested = json.loads((ROOT / "build/tests/manifest.json").read_text())
    assert tested["passed"] and tested["address_sanitizer"]
    image = validate_uf2(ROOT / "build" / args.target / uf2_name(args.target), TARGETS[args.target]["application_start"])
    assert image["sha256"] == manifest["sha256"]
    for name, expected in manifest["source_sha256"].items():
        actual = hashlib.sha256((ROOT / "SCReceiver" / name).read_bytes()).hexdigest()
        assert actual == expected, f"Rebuild changed source: {name}"
    python_files = list((ROOT / "tools").glob("*.py")) + list((ROOT / "tests").glob("*.py"))
    for path in python_files:
        ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for path in [ROOT / "README.md", *(ROOT / "docs").glob("*.md")]:
        for link in re.findall(r"\]\(([^)]+)\)", path.read_text(encoding="utf-8")):
            if "://" not in link:
                assert (path.parent / link).is_file(), link
    result = {"passed": True, "target": args.target, "release_version": manifest.get("release_version"),
              "firmware_source_files": len(manifest["source_sha256"]),
              "python_syntax_files": len(python_files), "uf2_sha256": image["sha256"],
              "address_sanitizer_passed": tested["address_sanitizer"],
              "physical_device_tested": False}
    browser = ROOT / "test-results/.last-run.json"
    if browser.is_file():
        report = json.loads(browser.read_text())
        assert report["status"] == "passed" and not report["failedTests"]
        result["saved_browser_suite_passed"] = True
        result["browser_hid_peer"] = "simulated"
    destination = "verification.json" if args.target == "xiao" else f"verification-{args.target}.json"
    (ROOT / "build" / destination).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
