import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("image", ROOT / "tools/image.py")
image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(image)


class ImageTests(unittest.TestCase):
    def memory(self):
        data = struct.pack("<II", 0x20040000, 0x27009) + bytes(range(8, 64))
        return {image.APP_START + i: value for i, value in enumerate(data)}

    def test_roundtrip(self):
        with tempfile.TemporaryDirectory(prefix=".image-test-", dir=ROOT) as directory:
            path = Path(directory) / "app.uf2"
            result = image.write_uf2(self.memory(), path)
            self.assertEqual(result["application_start"], "0x27000")
            self.assertEqual(result["uf2_blocks"], 1)
            self.assertEqual(image.validate_uf2(path), result)

    def test_protected_regions(self):
        for address in (0, 0x26000, image.STORE_START, 0xF4000, 0x10001000):
            memory = self.memory()
            memory[address] = 0
            with self.assertRaises(ValueError):
                image.validate_memory(memory)

    def test_bad_vectors(self):
        for sp, reset in ((0x10002000, 0x27009), (0x20040000, 0x27008),
                          (0x20040000, 0x26009), (0x20040001, 0x27009)):
            memory = self.memory()
            for i, value in enumerate(struct.pack("<II", sp, reset)):
                memory[image.APP_START + i] = value
            with self.assertRaises(ValueError):
                image.validate_memory(memory)

    def test_corrupt_uf2(self):
        with tempfile.TemporaryDirectory(prefix=".image-test-", dir=ROOT) as directory:
            path = Path(directory) / "app.uf2"
            image.write_uf2(self.memory(), path)
            good = path.read_bytes()
            for offset in (0, 4, 8, 12, 16, 20, 24, 28, 508):
                broken = bytearray(good)
                broken[offset] ^= 1
                path.write_bytes(broken)
                with self.assertRaises(ValueError):
                    image.validate_uf2(path)
            path.write_bytes(good[:-1])
            with self.assertRaises(ValueError):
                image.validate_uf2(path)


if __name__ == "__main__":
    unittest.main()
