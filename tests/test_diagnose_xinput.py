"""Read-only Windows probe tests, with no device/driver dependency."""

import ctypes
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("diagnose_xinput", TOOLS / "diagnose_xinput.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
sys.path.pop(0)


class XInputProbeTests(unittest.TestCase):
    def test_guide_extended_state_is_distinct_from_masked_standard_state(self):
        library = Mock()
        library.XInputGetState.return_value = 0
        library.XInputGetCapabilities.return_value = 1167
        def extended(index, pointer):
            data = ctypes.cast(pointer, ctypes.POINTER(probe.State)).contents
            data.gamepad.buttons = 0x0400 if index == 0 else 0
            return 0 if index < 2 else 1167
        library._guide_state = extended
        rows = probe.sample_xinput(library)
        self.assertEqual(rows[0]["gamepad"]["buttons"], 0)
        self.assertTrue(rows[0]["guide"]["pressed"])
        self.assertFalse(rows[1]["guide"]["pressed"])
        self.assertIsNone(rows[2]["guide"]["pressed"])

    def test_windows_abi_and_error_codes(self):
        self.assertEqual([ctypes.sizeof(t) for t in (probe.Gamepad, probe.State, probe.Capabilities)], [12, 16, 20])
        library = Mock()

        def state(index, pointer):
            if index == 0:
                data = ctypes.cast(pointer, ctypes.POINTER(probe.State)).contents
                data.packet_number = 42; data.gamepad.left_x = -32768; data.gamepad.buttons = 0x1000
            return [0, 1167, 5, 1167][index]

        library.XInputGetState.side_effect = state
        library.XInputGetCapabilities.return_value = 1167
        rows = probe.sample_xinput(library)
        self.assertEqual([r["connected"] for r in rows], [True, False, None, False])
        self.assertEqual(rows[0]["gamepad"]["left_x"], -32768)
        self.assertEqual(rows[0]["packet_number"], 42)
        self.assertTrue(all(r["device_attribution"] == "unknown" for r in rows))
        self.assertNotIn("gamepad", rows[2])
        self.assertEqual(rows[2]["state_result"], 5)
        self.assertFalse(library.XInputSetState.called)

    def test_no_device_capture_never_opens_hid(self):
        library = Mock()
        library.XInputGetState.return_value = library.XInputGetCapabilities.return_value = 1167
        with patch.object(probe.sys, "platform", "win32"), patch.object(probe, "read_pnp", return_value={"devices": []}), \
                patch.object(probe, "load_xinput", return_value=(library, "test", {})), \
                patch.object(probe, "open_receiver") as open_receiver, patch.object(probe.time, "sleep"):
            result = probe.capture(2, 0.05)
        self.assertEqual(result["errors"], {})
        self.assertEqual(len(result["samples"]), 2)
        self.assertEqual(result["pnp"]["devices"], [])
        open_receiver.assert_not_called()

    def test_partial_capture_preserves_driver_and_hid_errors(self):
        device = Mock()
        with patch.object(probe.sys, "platform", "win32"), patch.object(probe, "read_pnp", side_effect=OSError("PnP unavailable")), \
                patch.object(probe, "load_xinput", return_value=(None, None, {})), \
                patch.object(probe, "open_receiver", return_value=(device, 14)), \
                patch.object(probe.receiverctl, "exchange", side_effect=OSError("device removed")) as exchange, \
                patch.object(probe.time, "sleep"):
            result = probe.capture(2, 0.05, "explicit-path")
        self.assertEqual(set(result["errors"]), {"pnp", "xinput", "receiver"})
        self.assertEqual(result["samples"][0]["receiver_error"], "device removed")
        self.assertEqual(exchange.call_count, 1)
        device.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
