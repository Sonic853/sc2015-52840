"""Validate compiled-byte audit independently of the C++ descriptor producer."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from build import validate_xinput_descriptor, validate_ps5_descriptor
sys.path.pop(0)


class DescriptorTests(unittest.TestCase):
    def test_ps5_payload_sizes_and_report_ids(self):
        data = bytes([5,1,9,5,0xA1,1,0x75,8])
        for report_id, count, main in [(1,63,0x81),(2,47,0x91),(3,47,0xB1),(5,40,0xB1),(9,19,0xB1),(0x20,63,0xB1)]:
            data += bytes([0x85,report_id,0x95,count,main,2])
        data += bytes([0xC0])
        validate_ps5_descriptor(data)
        for offset in (9,11,15,17,21,23,27,29,33,35,39,41):
            changed = bytearray(data); changed[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                validate_ps5_descriptor(changed)
        with self.assertRaises(ValueError):
            validate_ps5_descriptor(data[:-2])

    def test_xusb_report_counts_and_endpoint_consistency(self):
        data = bytes.fromhex("09 04 00 00 02 ff 5d 01 00 "
                             "11 21 00 01 01 25 81 14 00 00 00 00 13 02 08 00 00 "
                             "07 05 81 03 20 00 01 07 05 02 03 20 00 08 "
                             "09 04 01 00 04 ff 5d 03 00 "
                             "1b 21 00 01 01 01 83 40 01 04 20 16 85 00 00 00 00 00 00 16 06 00 00 00 00 00 00 "
                             "07 05 83 03 20 00 02 07 05 04 03 20 00 04 07 05 85 03 20 00 40 07 05 06 03 20 00 10 "
                             "09 04 02 00 01 ff 5d 02 00 09 21 00 01 01 22 86 03 00 07 05 86 03 20 00 10 "
                             "09 04 03 00 00 ff fd 13 04 06 41 00 01 01 03")
        validate_xinput_descriptor(data)
        old = data[:20] + data[21:26] + b"\0" + data[26:]
        with self.assertRaisesRegex(ValueError, "five report lengths"):
            validate_xinput_descriptor(old)
        for offset in (0, 4, 12, 15, 20, 22, 25, 28, 30, 35, 37, 39):
            bad = bytearray(data); bad[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                validate_xinput_descriptor(bad)
        with self.assertRaises(ValueError):
            validate_xinput_descriptor(data[:-1])


if __name__ == "__main__":
    unittest.main()
