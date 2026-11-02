"""Install pinned build dependencies locally; never flash a device."""

import argparse
import hashlib
import os
from pathlib import Path
import platform
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / ".tools"
CORE_VERSION = "1.1.13"
USB_VERSION = "3.6.0"
BOARD_INDEX = "https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json"


def environment():
    env = os.environ.copy()
    env["ARDUINO_DIRECTORIES_DATA"] = str(TOOLS / "arduino-data")
    env["ARDUINO_DIRECTORIES_DOWNLOADS"] = str(TOOLS / "downloads")
    env["ARDUINO_DIRECTORIES_USER"] = str(TOOLS / "arduino-user")
    return env


def fetch(url, destination):
    request = urllib.request.Request(url, headers={"User-Agent": "SC2015Receiver-build/0.1"})
    with urllib.request.urlopen(request, timeout=120) as source:
        data = source.read()
    destination.write_bytes(data)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path)
    args = parser.parse_args()
    TOOLS.mkdir(exist_ok=True)
    cli = args.cli or TOOLS / "arduino-cli.exe"
    if not cli.is_file():
        if platform.system() != "Windows":
            raise SystemExit("Supply --cli /path/to/arduino-cli on this platform")
        base = "https://downloads.arduino.cc/arduino-cli/"
        archive_name = "arduino-cli_1.3.1_Windows_64bit.zip"
        # Arduino's v1.3.1 GitHub release asset digest, not an unpinned latest URL.
        expected = "cfece6f356fdc9ca003cc3f0a488470030719c8e0e7bfce5e42ac9410d87441f"
        archive = TOOLS / archive_name
        if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
            fetch(base + archive_name, archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
            raise SystemExit("Arduino CLI checksum mismatch")
        with zipfile.ZipFile(archive) as package:
            cli.write_bytes(package.read("arduino-cli.exe"))
    env = environment()
    commands = [
        [str(cli), "core", "update-index", "--additional-urls", BOARD_INDEX],
        [str(cli), "core", "install", f"Seeeduino:nrf52@{CORE_VERSION}", "--additional-urls", BOARD_INDEX],
        [str(cli), "lib", "install", f"Adafruit TinyUSB Library@{USB_VERSION}", "--no-deps"],
    ]
    for command in commands:
        subprocess.run(command, env=env, cwd=ROOT, check=True)
    print("Local Arduino CLI, Seeed nRF52 core and TinyUSB are ready.")


if __name__ == "__main__":
    main()
