"""Compile the production protocol core natively and test independent fixtures."""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import zlib

from Crypto.Cipher import AES

ROOT = Path(__file__).resolve().parents[1]


def fixtures(output):
    rng = random.Random(0x53433135)
    data = bytearray(struct.pack("<I", 108))
    for size in range(1, 28):
        for direction in range(2):
            for header in (0x01, 0xf7):
                key = rng.randbytes(16)
                iv = rng.randbytes(8)
                counter = rng.getrandbits(39)
                prefix = bytearray(counter.to_bytes(5, "little"))
                prefix[4] |= direction << 7
                plaintext = rng.randbytes(size)
                cipher = AES.new(key, AES.MODE_CCM, nonce=bytes(prefix) + iv, mac_len=4)
                cipher.update(bytes([header & 0x83]))
                ciphertext, mic = cipher.encrypt_and_digest(plaintext)
                packet = bytes([header]) + ciphertext + mic
                data.extend(key + iv + struct.pack("<QBBB", counter, direction, header, size))
                data.extend(plaintext + bytes([len(packet)]) + packet)
    (output / "crypto.bin").write_bytes(data)
    hops = base64.b64decode((ROOT / "tests/hop_vectors.b64").read_text().strip(), validate=True)
    if len(hops) != 128 * 12:
        raise ValueError("Invalid frozen hop fixture length")
    (output / "hops.bin").write_bytes(hops)
    capture = json.loads((ROOT / "tests/haptic_arrivals.json").read_text(encoding="utf-8"))
    if capture["format"] != "sc2015-haptic-arrival-fixture" or capture["version"] != 1:
        raise ValueError("Unexpected haptic fixture format")
    variants = [bytes.fromhex(value) for value in capture["variants"]]
    if not variants or any(len(v) != 10 or v[:2] != b"\x8f\x08" for v in variants):
        raise ValueError("Invalid haptic fixture command")
    recorded = bytearray(struct.pack("<I", len(variants)) + b"".join(variants))
    if [case["count"] for case in capture["cases"]] != [278, 371]:
        raise ValueError("Unexpected haptic trace selection")
    recorded.extend(struct.pack("<I", len(capture["cases"])))
    for case in capture["cases"]:
        payload = zlib.decompress(base64.b64decode("".join(case["data"]), validate=True))
        if len(payload) != case["count"] * 5 or hashlib.sha256(payload).hexdigest() != case["packed_sha256"]:
            raise ValueError("Haptic fixture integrity mismatch")
        events = list(struct.iter_unpack("<IB", payload))
        if events != sorted(events) or events[0][0] != 0 or events[-1][0] != case["span_us"] or any(v >= len(variants) for _, v in events):
            raise ValueError("Invalid haptic fixture event order/index")
        recorded.extend(struct.pack("<I", case["count"]))
        recorded.extend(payload)
    (output / "haptic_load.bin").write_bytes(recorded)


def compiler_environment(vcvars):
    command = f'call "{vcvars}" x64 >nul && set'
    env = {key.upper(): value for key, value in os.environ.items()}
    env["VSCMD_SKIP_SENDTELEMETRY"] = "1"
    raw = subprocess.check_output(f'cmd.exe /d /s /c "{command}"', env=env)
    for line in raw.decode("mbcs").splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1)
            env[key.upper()] = value
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true", help="Enable MSVC AddressSanitizer")
    parser.add_argument("--vcvars", type=Path, default=Path(
        r"C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"))
    args = parser.parse_args()
    output = ROOT / "build/tests"
    output.mkdir(parents=True, exist_ok=True)
    fixtures(output)
    sources = [ROOT / "tests/protocol_test.cpp", ROOT / "SCReceiver/protocol.cpp",
               ROOT / "SCReceiver/crypto.cpp", ROOT / "SCReceiver/usb.cpp", ROOT / "tests/usb_test.cpp",
               ROOT / "tests/compatibility_test.cpp", ROOT / "tests/radio_policy_test.cpp",
               ROOT / "tests/desktop_test.cpp", ROOT / "tests/queue_metrics_test.cpp",
               ROOT / "tests/queue_usb_test.cpp", ROOT / "SCReceiver/input_mode.cpp",
                ROOT / "SCReceiver/xinput.cpp", ROOT / "SCReceiver/rumble.cpp", ROOT / "SCReceiver/ps5.cpp",
                 ROOT / "SCReceiver/switch_pro.cpp", ROOT / "SCReceiver/motion.cpp", ROOT / "tests/input_mode_test.cpp", ROOT / "tests/ps5_test.cpp", ROOT / "tests/switch_test.cpp",
                 ROOT / "tests/motion_test.cpp", ROOT / "SCReceiver/lizard.cpp", ROOT / "tests/lizard_test.cpp", ROOT / "SCReceiver/right_stick.cpp", ROOT / "tests/right_stick_test.cpp",
                 ROOT / "SCReceiver/touch_feedback.cpp", ROOT / "tests/touch_feedback_test.cpp"]
    if os.name == "nt":
        env = compiler_environment(args.vcvars)
        executable = output / "protocol_test.exe"
        compiler = shutil.which("cl.exe", path=env["PATH"])
        if not compiler:
            raise SystemExit("vcvarsall did not provide a native C++ compiler")
        command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/Od", "/Zi",
                   f"/I{ROOT / 'tests/stubs'}", f"/I{ROOT / 'SCReceiver'}",
                   *map(str, sources), f"/Fe:{executable}",
                   "/link", "bcrypt.lib"]
        if args.sanitize:
            command.insert(1, "/fsanitize=address")
    else:
        env = os.environ.copy()
        executable = output / "protocol_test"
        command = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-g", f"-I{ROOT / 'tests/stubs'}", f"-I{ROOT / 'SCReceiver'}",
                   *map(str, sources), "-lcrypto", "-o", str(executable)]
    subprocess.run(command, cwd=output, env=env, check=True)
    result = subprocess.run([str(executable), str(output / "crypto.bin"), str(output / "hops.bin"), str(output / "haptic_load.bin")],
                            cwd=output, env=env, text=True, capture_output=True)
    (output / "protocol_test.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    if result.returncode:
        raise SystemExit(result.returncode)
    subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py"],
                   cwd=ROOT, env=env, check=True)
    manifest = {"passed": True, "crypto_oracle": "PyCryptodome AES.MODE_CCM",
                "aes_backend": "Windows BCrypt ECB" if os.name == "nt" else "OpenSSL ECB",
                "crypto_fixtures": 108, "hop_fixtures": 128,
                "address_sanitizer": args.sanitize or os.name != "nt",
                "hardware_interoperability_tested": False}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
