"""Plan, package and explicitly publish a two-board calendar release via gh."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

from image import read_hex, validate_uf2
from release_version import next_version, release_day, validate_version
from targets import TARGETS, uf2_name

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def gh(*arguments):
    return subprocess.run(["gh", *arguments], check=True, text=True, encoding="utf-8", capture_output=True).stdout.strip()


def repository_name(value):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", value or ""):
        raise ValueError("Expected an explicit owner/repository")
    return value


def commit_id(value):
    if not re.fullmatch(r"[0-9a-f]{40}", value or ""):
        raise ValueError("Expected the exact 40-character source commit")
    return value


def existing_versions(repo):
    repository_name(repo)
    names = set()
    # Include every page, including drafts and tags that have no release yet.
    # Permission/network errors must abort, not be mistaken for an empty list.
    for collection, key in (("tags", "name"), ("releases", "tag_name")):
        pages = json.loads(gh("api", "--paginate", "--slurp", f"repos/{repo}/{collection}?per_page=100"))
        if not isinstance(pages, list) or not all(isinstance(page, list) for page in pages):
            raise ValueError("Unexpected GitHub pagination response")
        for page in pages:
            for row in page:
                if not isinstance(row, dict) or not isinstance(row.get(key), str):
                    raise ValueError("Unexpected GitHub version record")
                names.add(row[key])
    return names


def asset_stem(version, target):
    profile = TARGETS[target]
    return f"SCReceiver-{version}-{profile['asset_board']}-s140-{profile['softdevice']}"


def checked_build(root, target, version, commit):
    profile = TARGETS[target]
    directory = root / "build" / target
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    required = {"target": target, "board": profile["fqbn"], "core": profile["core"],
                "softdevice": profile["softdevice"], "release_version": version, "source_commit": commit}
    if any(manifest.get(key) != value for key, value in required.items()):
        raise ValueError(f"{target}: wrong board, version or source commit")
    files = {"uf2": directory / uf2_name(target), "hex": directory / "SCReceiver.ino.hex"}
    observed = validate_uf2(files["uf2"], profile["application_start"])
    if any(manifest.get(key) != value for key, value in observed.items()):
        raise ValueError(f"{target}: UF2 does not match build manifest")
    if sha256(files["hex"]) != manifest.get("hex_sha256"):
        raise ValueError(f"{target}: HEX digest mismatch")
    memory = read_hex(files["hex"], profile["application_start"])
    wire = files["uf2"].read_bytes()
    for offset in range(0, len(wire), 512):
        address = struct.unpack_from("<I", wire, offset + 12)[0]
        if wire[offset + 32:offset + 288] != bytes(memory.get(address + i, 255) for i in range(256)):
            raise ValueError(f"{target}: HEX and UF2 payloads disagree")
    sources = {p.name: sha256(p) for p in (root / "SCReceiver").iterdir() if p.is_file()}
    if not sources or manifest.get("source_sha256") != sources:
        raise ValueError(f"{target}: firmware source content changed since build")
    audit = manifest.get("binary_audit", {})
    for facet in ("radio_and_timer_vectors_bound", "ep0_status_completion_hook_bound",
                  "feature_set_readiness_hooks_bound", "usb_lifecycle_hook_bound",
                  "compiled_hid_report_descriptors_match_original", "xinput_and_ms_os_descriptors_verified",
                  "ps5_hid_report_ids_and_sizes_verified", "switch_hid_report_ids_and_sizes_verified",
                  "embedded_release_version_verified", "uf2_bootloader_entry_linked"):
        if audit.get(facet) is not True:
            raise ValueError(f"{target}: missing binary audit {facet}")
    return manifest, files


def package(root, output, version, commit):
    validate_version(version); commit_id(commit)
    if output.exists():
        raise ValueError("Release output directory must be absent; existing assets are never overwritten")
    builds = {target: checked_build(root, target, version, commit) for target in TARGETS}
    if len({manifest["firmware_revision"] for manifest, _ in builds.values()}) != 1:
        raise ValueError("Boards have different capability revisions")
    output.mkdir(parents=True)
    assets = {}
    lines = [f"# SC2015 Receiver {version}", "", f"Source commit: `{commit}`", "",
             "版本按北京时间（UTC+8）生成；同日后续版本使用 p1、p2……。",
             "日期版本已嵌入两份固件，独立于诊断协议的数字能力修订号。", "",
             "| 板型 | SoftDevice | 应用起点 | 固件下载 |", "|---|---|---|---|"]
    for target, (manifest, files) in builds.items():
        stem = asset_stem(version, target)
        for extension, source in files.items():
            name = f"{stem}.{extension}"
            shutil.copyfile(source, output / name)
            assets[name] = sha256(output / name)
        name = f"{stem}.manifest.json"
        (output / name).write_text(json.dumps({**manifest, "release_asset": f"{stem}.uf2"}, indent=2) + "\n", encoding="utf-8")
        assets[name] = sha256(output / name)
        profile = TARGETS[target]
        lines.append(f"| {profile['label']} | S140 {profile['softdevice']} | `{hex(profile['application_start'])}` | `{stem}.uf2` |")
    revision = next(iter(builds.values()))[0]["firmware_revision"]
    lines += ["", f"- Diagnostic capability revision: **{revision}**.",
              "- UF2均为对应板型重新链接的应用镜像，不包含SoftDevice、bootloader、配置或绑定页；两种板型不可混刷。",
              "- ProMicro目标为OpenPuck同类SuperMini/ProMicro、Adafruit UF2 bootloader + S140 6.1.1；XIAO为S140 7.3.0。",
              "- HEX为同一应用的备选格式；manifest记录工具链、源码和二进制审计。SHA256SUMS提供下载校验。",
              "- CI编译/审计通过不代表实物射频、引脚或USB兼容性已验证。"]
    (output / "RELEASE_NOTES.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (output / "SHA256SUMS").write_text("".join(f"{digest}  {name}\n" for name, digest in sorted(assets.items())), encoding="ascii")
    assets["SHA256SUMS"] = sha256(output / "SHA256SUMS")
    result = {"version": version, "source_commit": commit, "assets": assets,
              "notes_sha256": sha256(output / "RELEASE_NOTES.md")}
    (output / "package.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def publish(root, output, version, commit, repo):
    validate_version(version); commit_id(commit); repository_name(repo)
    actual_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=all"], cwd=root, text=True).strip()
    if actual_commit != commit or dirty:
        raise ValueError("Publication requires the exact clean selected checkout")
    packed = json.loads((output / "package.json").read_text(encoding="utf-8"))
    expected_names = {f"{asset_stem(version, target)}.{ext}" for target in TARGETS for ext in ("uf2", "hex", "manifest.json")} | {"SHA256SUMS"}
    if packed.get("version") != version or packed.get("source_commit") != commit or set(packed.get("assets", {})) != expected_names:
        raise ValueError("Release package identity or asset set mismatch")
    for name, digest in packed["assets"].items():
        if sha256(output / name) != digest:
            raise ValueError(f"Release asset changed: {name}")
    if sha256(output / "RELEASE_NOTES.md") != packed.get("notes_sha256"):
        raise ValueError("Release notes changed after packaging")
    if version in existing_versions(repo):
        raise ValueError("Version was claimed during the build; rerun the workflow for the next version")
    # Creating a new ref is an atomic reservation. A concurrent writer causes
    # failure, never a force-update of a tag or an upload over an old asset.
    gh("api", "--method", "POST", f"repos/{repo}/git/refs", "-f", f"ref=refs/tags/{version}", "-f", f"sha={commit}")
    gh("release", "create", version, *[str(output / name) for name in sorted(expected_names)],
       "--repo", repo, "--verify-tag", "--draft", "--title", version,
       "--notes-file", str(output / "RELEASE_NOTES.md"))
    gh("release", "edit", version, "--repo", repo, "--draft=false")
    return gh("release", "view", version, "--repo", repo, "--json", "url", "--jq", ".url")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("next", "package", "publish"))
    parser.add_argument("--repo", default=os.environ.get("GH_REPO"))
    parser.add_argument("--version", default=os.environ.get("RELEASE_VERSION"))
    parser.add_argument("--commit", default=os.environ.get("GITHUB_SHA"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/release")
    args = parser.parse_args()
    if args.operation == "next":
        version = next_version(release_day(), existing_versions(args.repo))
        print(version)
        if os.environ.get("GITHUB_OUTPUT"):
            with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
                output.write(f"version={version}\n")
    elif args.operation == "package":
        print(json.dumps(package(ROOT, args.output, args.version, args.commit), indent=2))
    else:
        url = publish(ROOT, args.output, args.version, args.commit, args.repo)
        print(url)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
                summary.write(f"## Published {args.version}\n\n{url}\n")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stderr or str(error), file=sys.stderr)
        raise SystemExit(error.returncode)
