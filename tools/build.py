"""Build the XIAO S140 7.3.0 receiver and verify its application-only UF2."""

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
import subprocess
import sys

from dependencies import CORE_VERSION, USB_VERSION, ROOT, TOOLS, environment
from image import read_hex, write_uf2, validate_uf2


def validate_xinput_descriptor(data):
    """Check MS-XUSBI table 46 fields and the associated USB endpoint records."""
    if len(data) != 144 or data[:9] != bytes([9, 4, 0, 0, 2, 0xFF, 0x5D, 1, 0]):
        raise ValueError("Invalid XInput interface header/length")
    xusb = data[9:26]
    if xusb[:5] != bytes([17, 0x21, 0, 1, 1]):
        raise ValueError("Invalid XUSB version/subtype")
    if struct.unpack_from("<H", xusb, 5)[0] != 0x8125 or list(xusb[7:12]) != [20, 0, 0, 0, 0]:
        raise ValueError("Invalid XUSB IN endpoint or five report lengths")
    if struct.unpack_from("<H", xusb, 12)[0] != 0x0213 or list(xusb[14:17]) != [8, 0, 0]:
        raise ValueError("Invalid XUSB OUT endpoint or three report lengths")
    for offset, endpoint, interval in ((26, 0x81, 1), (33, 2, 8)):
        if struct.unpack_from("<BBBBHB", data, offset) != (7, 5, endpoint, 3, 32, interval):
            raise ValueError("XInput endpoint descriptor disagrees with XUSB report table")
    interfaces = []
    offset = 0
    while offset < len(data):
        length, kind = data[offset:offset + 2]
        if length < 2 or offset + length > len(data):
            raise ValueError("Malformed Xbox descriptor span")
        if kind == 4:
            interfaces.append((offset, bytes(data[offset:offset + length])))
        offset += length
    expected = [(0, bytes([9,4,0,0,2,255,0x5D,1,0])), (40, bytes([9,4,1,0,4,255,0x5D,3,0])),
                (104, bytes([9,4,2,0,1,255,0x5D,2,0])), (129, bytes([9,4,3,0,0,255,0xFD,0x13,4]))]
    if interfaces != expected:
        raise ValueError("Xbox personality must contain exactly four vendor interfaces and no HID")


def hid_report_bits(data):
    """Independently count payload bits by HID report ID and main item type."""
    bits, offset, size, count, report_id = {}, 0, 0, 0, 0
    while offset < len(data):
        prefix = data[offset]; offset += 1
        if prefix == 0xFE:
            raise ValueError("Unexpected long HID item")
        length = (0, 1, 2, 4)[prefix & 3]
        if offset + length > len(data):
            raise ValueError("Truncated PS5 HID item")
        value = int.from_bytes(data[offset:offset + length], "little")
        offset += length
        kind, tag = (prefix >> 2) & 3, prefix >> 4
        if kind == 1:
            if tag == 7: size = value
            elif tag == 8: report_id = value
            elif tag == 9: count = value
        elif kind == 0 and tag in (8, 9, 11):
            key = (tag, report_id)
            bits[key] = bits.get(key, 0) + size * count
    return bits


def validate_ps5_descriptor(data):
    bits = hid_report_bits(data)
    expected = {(8, 1): 63 * 8, (9, 2): 47 * 8, (11, 3): 47 * 8,
                (11, 5): 40 * 8, (11, 9): 19 * 8, (11, 0x20): 63 * 8}
    if bits != expected or data[:6] != bytes([5,1,9,5,0xA1,1]) or data[-1] != 0xC0:
        raise ValueError("PS5 HID report IDs, lengths or collection disagree with the USB profile")


def validate_switch_descriptor(data):
    expected = {(8, report_id): 63 * 8 for report_id in (0x30, 0x21, 0x81)}
    expected.update({(9, report_id): 63 * 8 for report_id in (1, 0x10, 0x80, 0x82)})
    if hid_report_bits(data) != expected or data[:6] != bytes([5,1,9,4,0xA1,1]) or data[-1] != 0xC0:
        raise ValueError("Switch HID report IDs, lengths or collection disagree with the USB profile")


def audit(core, hex_path, elf_path):
    tool = TOOLS / "arduino-data/packages/Seeeduino/tools/arm-none-eabi-gcc/9-2019q4/bin/arm-none-eabi-nm.exe"
    if not tool.is_file():
        tool = tool.with_suffix("")
    raw = subprocess.check_output([str(tool), "-S", "-n", str(elf_path)], text=True)
    symbols, sizes = {}, {}
    for line in raw.splitlines():
        columns = line.split()
        if len(columns) == 4:
            sizes[columns[3]] = int(columns[1], 16)
            symbols[columns[3]] = (int(columns[0], 16), columns[2])
        elif len(columns) == 3:
            symbols[columns[2]] = (int(columns[0], 16), columns[1])
    memory = read_hex(hex_path)
    for name, irq in (("RADIO_IRQHandler", 1), ("TIMER3_IRQHandler", 26)):
        address, kind = symbols[name]
        if kind != "T":
            raise ValueError(f"Weak or absent interrupt handler: {name}")
        offset = 0x27000 + 4 * (16 + irq)
        vector = int.from_bytes(bytes(memory[offset + i] for i in range(4)), "little")
        if vector != address | 1:
            raise ValueError(f"Vector does not bind {name}")
    for name in ("dcd_edpt0_status_complete", "enterUf2Dfu", "__wrap_hidd_control_xfer_cb",
                 "__wrap_tud_control_xfer", "__wrap_usbd_control_xfer_cb",
                  "tud_event_hook_cb", "__wrap_tud_descriptor_bos_cb", "__wrap_tud_vendor_control_xfer_cb",
                  "__wrap_usbd_app_driver_get_cb", "__wrap_tud_descriptor_string_cb"):
        if symbols.get(name, (0, ""))[1] != "T":
            raise ValueError(f"Required strong USB/UF2 hook not linked: {name}")
    address = symbols["_usbd_driver"][0]
    drivers = bytes(memory[address + i] for i in range(sizes["_usbd_driver"]))
    callback = (symbols["__wrap_hidd_control_xfer_cb"][0] | 1).to_bytes(4, "little")
    if callback not in drivers:
        raise ValueError("TinyUSB class table bypasses the Feature readiness callback")
    if any(name.startswith("vendord_") or "desc_bos" in name for name in symbols):
        raise ValueError("Unexpected TinyUSB vendor/WebUSB capability in HID-only receiver")
    reference = json.loads((ROOT / "docs/provenance.json").read_text())
    for symbol, record in (("_ZN3usbL18desktop_descriptorE", 0), ("_ZN3usbL15slot_descriptorE", 1)):
        expected = bytes.fromhex(reference["hid_reports"][record]["bytes"])
        address = symbols[symbol][0]
        observed = bytes(memory[address + i] for i in range(len(expected)))
        if observed != expected:
            raise ValueError("Compiled HID descriptor differs from recovered firmware")
    symbol = "_ZN6xinputL10descriptorE"
    address = symbols[symbol][0]
    validate_xinput_descriptor(bytes(memory[address + i] for i in range(sizes[symbol])))
    symbol = "_ZN3ps5L17report_descriptorE"
    address = symbols[symbol][0]
    validate_ps5_descriptor(bytes(memory[address + i] for i in range(sizes[symbol])))
    symbol = "_ZN10switch_proL17report_descriptorE"
    address = symbols[symbol][0]
    validate_switch_descriptor(bytes(memory[address + i] for i in range(sizes[symbol])))
    xinput_tables = {
        "_ZN6xinputL13os_compatibleE": bytes.fromhex(
            "28 00 00 00 00 01 04 00 01 00 00 00 00 00 00 00 00 01") + b"XUSB10\0\0" + bytes(14),
        "_ZN6xinputL9os_stringE": bytes([18, 3]) + "MSFT100".encode("utf-16-le") + bytes([0x20, 0]),
    }
    for symbol, expected in xinput_tables.items():
        address = symbols[symbol][0]
        if sizes[symbol] != len(expected) or bytes(memory[address + i] for i in range(len(expected))) != expected:
            raise ValueError(f"Unexpected compiled XInput/MS OS descriptor: {symbol}")
    return {"radio_and_timer_vectors_bound": True,
            "ep0_status_completion_hook_bound": True,
            "uf2_bootloader_entry_linked": True,
            "feature_set_readiness_hooks_bound": True,
            "usb_lifecycle_hook_bound": True,
            "webusb_vendor_class_disabled": True,
            "compiled_hid_report_descriptors_match_original": True,
            "xinput_class_driver_linked": True,
            "xinput_and_ms_os_descriptors_verified": True,
            "ps5_hid_report_ids_and_sizes_verified": True,
            "switch_hid_report_ids_and_sizes_verified": True,
            "flash_guard": "0xE9000..0xECFFF mapping/bond pages excluded",
            "softdevice_enable_requested_by_application": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=TOOLS / "arduino-cli.exe")
    parser.add_argument("--verify", type=Path)
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()
    if args.verify:
        print(json.dumps(validate_uf2(args.verify), indent=2))
        return
    env = environment()
    core = TOOLS / "arduino-data/packages/Seeeduino/hardware/nrf52" / CORE_VERSION
    usb = TOOLS / "arduino-user/libraries/Adafruit_TinyUSB_Library"
    if not args.cli.is_file() or not (core / "platform.txt").is_file():
        raise SystemExit("Run python tools/dependencies.py first")
    if f"version={USB_VERSION}" not in (usb / "library.properties").read_text():
        raise SystemExit("Unexpected TinyUSB version")
    board = json.loads(subprocess.check_output([
        str(args.cli), "board", "details", "-b", "Seeeduino:nrf52:xiaonRF52840", "--format", "json"
    ], env=env, text=True))
    properties = dict(item.split("=", 1) for item in board["build_properties"])
    if (board["version"] != CORE_VERSION or properties["build.sd_version"] != "7.3.0"
            or properties["build.ldscript"] != "nrf52840_s140_v7.ld"):
        raise SystemExit("Unexpected board/SoftDevice/linker layout")
    output = ROOT / "build/xiao"
    output.mkdir(parents=True, exist_ok=True)
    flags = ("-DNRF52840_XXAA {build.flags.usb} -DCFG_TUD_HID=5 -DCFG_TUD_VENDOR=0 "
             "-DCFG_TUD_TASK_QUEUE_SZ=512 -DSC2015_RECEIVER=1")
    includes = f'-I"{(usb / "src/arduino").as_posix()}" -fstack-usage'
    link = ('-Wl,--wrap=tud_descriptor_device_cb,--wrap=hidd_control_xfer_cb,'
            '--wrap=tud_control_xfer,--wrap=usbd_control_xfer_cb,'
            '--wrap=tud_descriptor_bos_cb,--wrap=tud_vendor_control_xfer_cb,--wrap=usbd_app_driver_get_cb,'
            '--wrap=tud_descriptor_string_cb '
            f'"-Wl,-T,{(ROOT / "tools/store_guard.ld").as_posix()}"')
    command = [str(args.cli), "compile", "-b", "Seeeduino:nrf52:xiaonRF52840",
               "--build-path", str(ROOT / "build/cache"), "--output-dir", str(output),
               "--library", str(usb), "--warnings", "all",
               "--build-property", f"build.extra_flags={flags}",
               "--build-property", f"compiler.c.extra_flags={includes}",
               "--build-property", f"compiler.cpp.extra_flags={includes}",
               "--build-property", f"compiler.c.elf.extra_flags={link}", str(ROOT / "SCReceiver")]
    if args.clean:
        command.insert(2, "--clean")
    with (output / "compile.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print(f"Compilation failed; see {output / 'compile.log'}")
        raise SystemExit(result.returncode)
    hex_path = output / "SCReceiver.ino.hex"
    uf2 = output / "SCReceiver-xiao-s140-7.3.0.uf2"
    info = write_uf2(read_hex(hex_path), uf2)
    info["binary_audit"] = audit(core, hex_path, output / "SCReceiver.ino.elf")
    info.update({"board": "Seeeduino:nrf52:xiaonRF52840", "core": CORE_VERSION,
                 "tinyusb": USB_VERSION, "hardware_tested": False,
                 "source_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                    for p in sorted((ROOT / "SCReceiver").glob("*")) if p.is_file()}})
    info["firmware_revision"] = int(re.search(r"FIRMWARE_REVISION\s*=\s*(\d+)",
        (ROOT / "SCReceiver/radio_policy.h").read_text()).group(1))
    info["diagnostics_protocol"] = 3
    (output / "manifest.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(info, indent=2))
    print(uf2)


if __name__ == "__main__":
    main()
