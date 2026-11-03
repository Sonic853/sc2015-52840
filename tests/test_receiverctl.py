"""Offline FC wire/reader regressions; no hidapi import or device access."""

import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("receiverctl", ROOT / "tools/receiverctl.py")
ctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ctl)


class Pages:
    def __init__(self, expirations=0, mutate=None, count=3):
        self.token = 0
        self.expirations = expirations
        self.mutate = mutate
        self.count = count
        self.requests = []

    def exchange(self, device, command, arguments=b""):
        assert command == 0xFC
        page = arguments[0] if arguments else 0
        self.requests.append(page)
        status = 0
        if page == 0:
            self.token += 1
        else:
            assert arguments == bytes([page]) + struct.pack("<I", self.token)
            if page == 5 and self.expirations:
                self.expirations -= 1
                self.token += 1
                status = 2
        reply = bytearray(64)
        reply[:6] = bytes([0xFC, 62, 1, page, 11, status])
        struct.pack_into("<II", reply, 6, self.token, 0xFFFFFFF0)
        if page == 0:
            reply[14:18] = bytes([12, 31, self.count, 20])
            struct.pack_into("<10I", reply, 18, *range(10))
            struct.pack_into("<I", reply, 58, 8000)
            reply[62:64] = bytes([8, 1])
        elif page <= 8:
            struct.pack_into("<12I", reply, 14, *range(self.token * 1000 + page * 20, self.token * 1000 + page * 20 + 12))
            reply[62:64] = bytes([8, 2])
        elif page == 17:
            struct.pack_into("<12I", reply, 14, *range(200, 212))
            reply[62:64] = bytes([9, 4])
        else:
            first = (page - 9) * 2
            reply[62] = min(2, self.count - first)
            for index in range(reply[62]):
                ordinal = first + index
                struct.pack_into("<4I8B", reply, 14 + index * 24,
                                 (0xFFFFFFFF + ordinal) & 0xFFFFFFFF, 0xFFFFFFE0 + ordinal,
                                 7, 8000 + ordinal, 2, 7, 0x8F, 10, 1, 12, 10, 3)
        if self.mutate:
            self.mutate(page, reply)
        return bytes(reply)


class ReceiverCtlTests(unittest.TestCase):
    def test_input_mode_extension_and_old_firmware(self):
        reply = bytearray(64)
        reply[:11] = bytes([0xFD, 62, 1, 14, 0, 1, 1, 255, 5, 1, 7])
        struct.pack_into("<10I", reply, 12, *range(10))
        reply[52:60] = bytes.fromhex("a101000100001400")
        parsed = ctl.parse_input_mode(reply, 14)
        self.assertEqual(parsed["xinput"]["in_completed"], 5)
        self.assertEqual(parsed["xinput"]["last_setup"], "a101000100001400")
        self.assertIsNone(parsed["owner_slot"])
        self.assertTrue(parsed["xinput"]["opened"])
        for offset, value in [(9, 0), (9, 2), (10, 16), (11, 1), (3, 13), (5, 2), (7, 4)]:
            bad = reply.copy(); bad[offset] = value
            with self.subTest(offset=offset, value=value), self.assertRaises(OSError):
                ctl.parse_input_mode(bad, 14)
        reply[3] = 13; reply[9:] = bytes(55)
        self.assertIsNone(ctl.parse_input_mode(reply, 13)["xinput"])

    def test_both_personalities_list_only_vendor_hids(self):
        class Hid:
            @staticmethod
            def enumerate(vendor, product):
                assert vendor == 0x28DE
                first = 1 if product == 0x1142 else 2
                return [{"usage_page": 1, "usage": 6, "interface_number": first - 1},
                        {"usage_page": 0xFF00, "usage": 1, "interface_number": first},
                        {"usage_page": 0, "interface_number": first + 3}]
        entries = ctl.receiver_interfaces(Hid)
        self.assertEqual([e["interface_number"] for e in entries], [1, 4, 2, 5])

    def read(self, peer, detail=True):
        with patch.object(ctl, "exchange", side_effect=peer.exchange):
            return ctl.read_queue_diagnostics(object(), detail)

    def test_summary_and_complete_wire_snapshot(self):
        summary_peer = Pages()
        summary = self.read(summary_peer, False)
        self.assertEqual(summary_peer.requests, [0])
        self.assertFalse(summary["detail"])
        self.assertEqual(summary["snapshot_clock_us"], 0xFFFFFFF0)
        self.assertEqual(summary["haptic_wait_budget_us"], 8000)
        peer = Pages()
        full = self.read(peer)
        self.assertEqual(peer.requests, [0, *range(1, 9), 17, 9, 10])
        self.assertEqual(full["slots"][0]["admitted"], 1020)
        self.assertEqual(full["slots"][3]["usb_haptic_reset_cancelled"], 211)
        self.assertEqual([e["id"] for e in full["events"]], [0xFFFFFFFF, 0, 1])
        self.assertEqual(full["events"][0]["kind_name"], "UsbTimeout")
        self.assertEqual(full["events"][0]["command"], 0x8F)
        self.assertEqual(full["usb_depth"], 9)
        self.assertTrue(full["captured_at"].endswith("+00:00"))
        empty = Pages(count=0)
        self.assertEqual(self.read(empty)["events"], [])
        self.assertEqual(empty.requests, [0, *range(1, 9), 17])

    def test_expired_read_restarts_without_mixing_earlier_pages(self):
        peer = Pages(expirations=1)
        snapshot = self.read(peer)
        self.assertEqual(peer.requests.count(0), 2)
        self.assertEqual(snapshot["snapshot_id"], 3)
        self.assertEqual(snapshot["slots"][0]["admitted"], 3020)

    def test_repeated_expiry_is_bounded_and_returns_no_partial_result(self):
        peer = Pages(expirations=2)
        with self.assertRaises(ctl.QueueSnapshotExpiredError):
            self.read(peer)
        self.assertEqual(peer.requests.count(0), 2)

    def test_mixed_headers_slot_state_and_event_counts_are_rejected(self):
        for page, offset, value in [(2, 6, 9), (2, 10, 0), (2, 4, 10), (5, 62, 7), (9, 62, 3)]:
            with self.subTest(page=page, offset=offset):
                def mutate(selected, reply):
                    if selected == page:
                        reply[offset] = value
                peer = Pages(mutate=mutate)
                with self.assertRaises(OSError):
                    self.read(peer)
                self.assertEqual(peer.requests.count(0), 1)

    def identity_replies(self, revision=11, original=False):
        identity = bytearray(64)
        identity[:2] = bytes([0xA1, 34])
        identity[6:16] = b"f0d8bac40b"
        identity[16:27] = b"SC2015 XIAO" if not original else b"ORIGINAL   "
        diagnostic = bytearray(64)
        diagnostic[:3] = bytes([0xF0, 62, 3])
        diagnostic[34] = 4
        diagnostic[59:64] = bytes.fromhex("f0d8bac40b")
        link = bytearray(64)
        link[:4] = bytes([0xF7, 62, 1, revision])
        link[63] = revision
        return {0xA1: bytes(identity), 0xF0: bytes(diagnostic), 0xF7: bytes(link)}

    def test_identity_and_revision_gate_reject_original_and_old_firmware(self):
        for revision, original, expected in [(11, True, [0xA1]), (10, False, [0xA1, 0xF0, 0xF7])]:
            with self.subTest(revision=revision, original=original):
                replies = self.identity_replies(revision, original)
                with patch.object(ctl, "exchange", side_effect=lambda device, command: replies[command]) as exchange:
                    with self.assertRaises(OSError):
                        ctl.confirm_queue_support(object())
                    self.assertEqual([c.args[1] for c in exchange.call_args_list], expected)

    def test_confirmed_identity_accepts_revision11_and_rejects_mismatch(self):
        replies = self.identity_replies()
        with patch.object(ctl, "exchange", side_effect=lambda device, command: replies[command]) as exchange:
            ctl.confirm_queue_support(object())
            self.assertEqual([c.args[1] for c in exchange.call_args_list], [0xA1, 0xF0, 0xF7])
            mismatch = bytearray(replies[0xF0])
            mismatch[59] ^= 8
            replies[0xF0] = bytes(mismatch)
            with self.assertRaises(OSError):
                ctl.confirm_queue_support(object())


if __name__ == "__main__":
    unittest.main()
