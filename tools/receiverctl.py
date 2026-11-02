"""HID control/diagnostics for an explicitly selected SC2015 receiver interface."""

import argparse
import json
import struct
import time


def normalize(data):
    data = bytes(data)
    if len(data) == 65 and data[0] == 0:
        data = data[1:]
    if len(data) != 64:
        raise OSError(f"Expected 64-byte Feature data, got {len(data)}")
    return data


def exchange(device, command, arguments=b"", read=True):
    if len(arguments) > 62:
        raise ValueError("Feature arguments exceed 62 bytes")
    report = bytes([0, command, len(arguments)]) + arguments
    report = report.ljust(65, b"\x00")
    for _ in range(50):
        try:
            if device.send_feature_report(report) >= 0:
                break
        except OSError:
            pass
        time.sleep(0.005)
    else:
        raise OSError("SET_FEATURE did not complete")
    if not read:
        return None
    for _ in range(50):
        time.sleep(0.005)
        try:
            reply = normalize(device.get_feature_report(0, 65))
        except OSError:
            continue
        ready = (command != 0xF0 or (reply[1] >= 57 and reply[2] in (1, 2, 3)))
        ready &= command != 0xB4 or reply[1] == 1
        ready &= command != 0xF7 or (reply[1] == 62 and reply[2] == 1)
        if reply[0] == command and ready:
            return reply
    raise OSError("GET_FEATURE did not return the requested command")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["list", "status", "pair", "unpair-mode", "commit", "forget", "capture"])
    parser.add_argument("--path", help="Exact path printed by list; selects one of interfaces 1..4")
    parser.add_argument("--seconds", type=float, default=10)
    args = parser.parse_args()
    try:
        import hid
    except ImportError:
        raise SystemExit("Install the host helper with: python -m pip install hidapi")
    devices = [entry for entry in hid.enumerate(0x28DE, 0x1142)
               if entry.get("usage_page") == 0xFF00 or 1 <= entry.get("interface_number", -1) <= 4]
    if args.action == "list":
        for entry in devices:
            path = entry["path"].decode("utf-8")
            print(json.dumps({"path": path, "interface": entry.get("interface_number"),
                              "product": entry.get("product_string")}, ensure_ascii=False))
        return
    if not args.path:
        parser.error("Use list, then supply an explicit --path")
    path = args.path.encode("utf-8")
    if not any(entry["path"] == path for entry in devices):
        raise SystemExit("Selected vendor HID interface was not found")
    device = hid.device()
    device.open_path(path)
    try:
        if args.action == "status":
            reply = exchange(device, 0xF0)
            if reply[1] < 57 or reply[2] not in (1, 2, 3):
                raise OSError("Selected firmware does not expose supported SC2015 diagnostics")
            names = ["rx", "authenticated", "mic_failures", "duplicates", "invalid",
                     "queue_full", "storage_failures"]
            result = dict(zip(names, struct.unpack_from("<7I", reply, 5)))
            hardware = ["crc_ok", "crc_bad", "radio_queue_full", "late_ack", "radio_faults", "usb_command_drops"]
            result.update(zip(hardware, struct.unpack_from("<6I", reply, 35)))
            result.update(active_pipes=hex(reply[3]), pairing=bool(reply[4]))
            if reply[2] >= 2:
                if reply[1] < 62 or reply[33] >= 4 or reply[34] != 4:
                    raise OSError("Invalid diagnostics interface identity")
                result.update(slot=reply[33], receiver_id=reply[59:64].hex())
            if reply[2] >= 3:
                link = exchange(device, 0xF7)
                result["pairing_link"] = {
                    "firmware_revision": link[3], "pipe_rx": list(struct.unpack_from("<6I", link, 12)),
                    "ack_scheduled": struct.unpack_from("<I", link, 36)[0],
                    "ack_ready": struct.unpack_from("<I", link, 40)[0],
                    "ack_completed": struct.unpack_from("<I", link, 44)[0],
                    "ack_skipped": struct.unpack_from("<I", link, 48)[0],
                    "last_pipe": link[4], "last_channel": link[5], "last_size": link[6],
                    "last_s1": link[7], "last_header": link[8], "last_ack_length": link[9],
                    "max_irq_delay_us": struct.unpack_from("<H", link, 56)[0],
                    "last_tx_ready_delay_us": struct.unpack_from("<H", link, 58)[0],
                }
            result["slot_state"] = exchange(device, 0xB4)[2]
            print(json.dumps(result, indent=2))
        elif args.action in ("pair", "unpair-mode"):
            exchange(device, 0xAD, bytes([args.action == "pair", 0]))
            print("Pairing enabled" if args.action == "pair" else "Pairing disabled")
        elif args.action == "commit":
            exchange(device, 0xB3)
            state = exchange(device, 0xB4)[2]
            print(json.dumps({"slot_state": state, "connected_nonpending": state == 2}))
        elif args.action == "forget":
            exchange(device, 0xAF, b"wipe")
            print("Receiver bond records cleared")
        elif args.action == "capture":
            if not 0 < args.seconds <= 3600:
                raise ValueError("Capture duration must be 0..3600 seconds")
            end = time.monotonic() + args.seconds
            while time.monotonic() < end:
                report = device.read(64, timeout_ms=100)
                if report:
                    print(json.dumps({"time": time.monotonic(), "data": bytes(report).hex()}), flush=True)
    finally:
        device.close()


if __name__ == "__main__":
    main()
