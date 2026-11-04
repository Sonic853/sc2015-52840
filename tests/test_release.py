"""Offline release/version/package tests. No GitHub writes or device access."""

from datetime import datetime, timezone
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import release
from release_version import next_version, release_day, validate_version
from image import write_uf2
from targets import TARGETS, uf2_name
sys.path.pop(0)

COMMIT = "a" * 40
VERSION = "2026.01.10"
AUDITS = ("radio_and_timer_vectors_bound", "ep0_status_completion_hook_bound", "feature_set_readiness_hooks_bound",
          "usb_lifecycle_hook_bound", "compiled_hid_report_descriptors_match_original", "xinput_and_ms_os_descriptors_verified",
          "ps5_hid_report_ids_and_sizes_verified", "switch_hid_report_ids_and_sizes_verified",
          "embedded_release_version_verified", "uf2_bootloader_entry_linked")


def hex_record(kind, address, payload):
    data = bytes([len(payload), address >> 8, address & 255, kind]) + payload
    return ":" + (data + bytes([-sum(data) & 255])).hex().upper() + "\n"


class VersionTests(unittest.TestCase):
    def test_calendar_and_numeric_patch_order(self):
        self.assertEqual(next_version(VERSION, []), VERSION)
        self.assertEqual(next_version(VERSION, [VERSION]), VERSION + "p1")
        self.assertEqual(next_version(VERSION, [VERSION, VERSION + "p1", VERSION + "p9", VERSION + "p10"]), VERSION + "p11")
        self.assertEqual(next_version(VERSION, [VERSION + "p3"]), VERSION + "p4")
        self.assertEqual(next_version(VERSION, ["v2026.01.10", "2026.01.09p7", "2026.01.10-rc1"]), VERSION)
        self.assertEqual(release_day(datetime(2026,1,9,16,0,tzinfo=timezone.utc)), VERSION)
        self.assertEqual(release_day(datetime(2026,1,9,15,59,tzinfo=timezone.utc)), "2026.01.09")

    def test_invalid_calendar_and_version_input(self):
        for value in (None, "", "2026.1.10", "2026.02.29", "2026.01.10p0", "2026.01.10p01", "2026.01.10p-1", "2026.01.10\n", "../2026.01.10"):
            with self.subTest(value=value), self.assertRaises(ValueError): validate_version(value)
        self.assertEqual(validate_version("2028.02.29p12"), "2028.02.29p12")
        with self.assertRaises(ValueError): next_version(VERSION + "p1", [])

    def test_tags_releases_drafts_and_pages_share_namespace(self):
        with patch.object(release, "gh", side_effect=[
                json.dumps([[{"name": VERSION}], [{"name": VERSION + "p2"}]]),
                json.dumps([[{"tag_name": VERSION + "p3", "draft": True}]])]) as gh:
            names = release.existing_versions("owner/repo")
            self.assertEqual(next_version(VERSION, names), VERSION + "p4")
            for call in gh.call_args_list:
                self.assertIn("--paginate", call.args); self.assertIn("--slurp", call.args)
        with patch.object(release, "gh", side_effect=subprocess.CalledProcessError(1, "gh")):
            with self.assertRaises(subprocess.CalledProcessError): release.existing_versions("owner/repo")
        with patch.object(release, "gh", return_value='{"message":"not a list"}'):
            with self.assertRaises(ValueError): release.existing_versions("owner/repo")


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix=".release-test-", dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "SCReceiver").mkdir()
        (self.root / "SCReceiver" / "fixture.cpp").write_bytes(b"// fixture\n")
        self.output = self.root / "build/release"
        for target, profile in TARGETS.items():
            directory = self.root / "build" / target; directory.mkdir(parents=True)
            start = profile["application_start"]
            payload = struct.pack("<II", 0x20040000, start + 9) + bytes(range(8,64))
            info = write_uf2({start + i: b for i,b in enumerate(payload)}, directory / uf2_name(target), start)
            path = directory / "SCReceiver.ino.hex"
            path.write_text(hex_record(4, 0, (start >> 16).to_bytes(2,"big")) + hex_record(0,start & 65535,payload) + hex_record(1,0,b""), encoding="ascii")
            self.save_manifest(target, {**info, "target":target, "board":profile["fqbn"], "core":profile["core"],
                "softdevice":profile["softdevice"], "release_version":VERSION, "source_commit":COMMIT,
                "firmware_revision":19, "hex_sha256":release.sha256(path),
                "source_sha256":{"fixture.cpp":release.sha256(self.root / "SCReceiver/fixture.cpp")},
                "binary_audit":dict.fromkeys(AUDITS, True)})

    def save_manifest(self, target, manifest):
        (self.root / "build" / target / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    def manifest(self, target="promicro"):
        return json.loads((self.root / "build" / target / "manifest.json").read_text())

    def pack(self):
        return release.package(self.root, self.output, VERSION, COMMIT)

    def test_two_boards_named_assets_and_checksums(self):
        result = self.pack()
        self.assertEqual(len(list(self.output.glob("*.uf2"))), 2)
        self.assertEqual(len(result["assets"]), 7)
        for name, digest in result["assets"].items(): self.assertEqual(release.sha256(self.output / name), digest)
        for target in TARGETS:
            self.assertTrue((self.output / (release.asset_stem(VERSION,target) + ".uf2")).is_file())
        self.assertEqual(len((self.output / "SHA256SUMS").read_text().splitlines()), 6)
        with self.assertRaises(ValueError): self.pack()

    def test_wrong_board_version_commit_or_missing_audit_fails_before_output(self):
        original = self.manifest()
        for field, value in (("target","xiao"),("board","bad"),("softdevice","7.3.0"),("release_version",VERSION+"p1"),("source_commit","b"*40),("binary_audit",{})):
            self.save_manifest("promicro", {**original, field:value})
            with self.subTest(field=field), self.assertRaises(ValueError): self.pack()
            self.assertFalse(self.output.exists())

    def test_changed_sources_and_firmware_rejected(self):
        source = self.root / "SCReceiver/fixture.cpp"; source.write_bytes(b"// changed\n")
        with self.assertRaises(ValueError): self.pack()
        source.write_bytes(b"// fixture\n")
        uf2 = self.root / "build/promicro" / uf2_name("promicro")
        data = bytearray(uf2.read_bytes()); data[50] ^= 1; uf2.write_bytes(data)
        with self.assertRaises(ValueError): self.pack()

    def test_hex_content_must_match_uf2_even_with_updated_hex_digest(self):
        path = self.root / "build/promicro/SCReceiver.ino.hex"
        payload = struct.pack("<II",0x20040000,0x26009) + bytes(56)
        path.write_text(hex_record(4,0,b"\0\2") + hex_record(0,0x6000,payload) + hex_record(1,0,b""), encoding="ascii")
        self.save_manifest("promicro", {**self.manifest(), "hex_sha256":release.sha256(path)})
        with self.assertRaisesRegex(ValueError,"payloads disagree"): self.pack()

    def test_publish_reserves_tag_then_uploads_draft_then_publishes(self):
        self.pack()
        with patch.object(release.subprocess,"check_output",side_effect=[COMMIT+"\n",""]), \
             patch.object(release,"existing_versions",return_value=set()), patch.object(release,"gh",return_value="release-url") as gh:
            self.assertEqual(release.publish(self.root,self.output,VERSION,COMMIT,"owner/repo"),"release-url")
            calls = [call.args for call in gh.call_args_list]
            self.assertEqual(calls[0][:4],("api","--method","POST","repos/owner/repo/git/refs"))
            self.assertIn("sha="+COMMIT,calls[0]); self.assertIn("--draft",calls[1]); self.assertIn("--verify-tag",calls[1])
            self.assertEqual(calls[2][:3],("release","edit",VERSION)); self.assertIn("--draft=false",calls[2])
            self.assertNotIn("--clobber",str(calls)); self.assertNotIn("--force",str(calls))

    def test_collision_and_failed_draft_never_overwrite_or_publish(self):
        self.pack()
        with patch.object(release.subprocess,"check_output",side_effect=[COMMIT,""]), \
             patch.object(release,"existing_versions",return_value={VERSION}), patch.object(release,"gh") as gh:
            with self.assertRaises(ValueError): release.publish(self.root,self.output,VERSION,COMMIT,"owner/repo")
            gh.assert_not_called()
        with patch.object(release.subprocess,"check_output",side_effect=[COMMIT,""]), \
             patch.object(release,"existing_versions",return_value=set()), \
             patch.object(release,"gh",side_effect=["",subprocess.CalledProcessError(1,"gh")]) as gh:
            with self.assertRaises(subprocess.CalledProcessError): release.publish(self.root,self.output,VERSION,COMMIT,"owner/repo")
            self.assertEqual(gh.call_count,2)

    def test_dirty_checkout_or_changed_assets_cannot_publish(self):
        self.pack()
        with patch.object(release.subprocess,"check_output",side_effect=[COMMIT," M changed.cpp"]), patch.object(release,"gh") as gh:
            with self.assertRaises(ValueError): release.publish(self.root,self.output,VERSION,COMMIT,"owner/repo")
            gh.assert_not_called()
        (self.output / "SHA256SUMS").write_text("changed")
        with patch.object(release.subprocess,"check_output",side_effect=[COMMIT,""]), patch.object(release,"gh") as gh:
            with self.assertRaises(ValueError): release.publish(self.root,self.output,VERSION,COMMIT,"owner/repo")
            gh.assert_not_called()


if __name__ == "__main__":
    unittest.main()
