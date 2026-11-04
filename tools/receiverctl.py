"""HID control/diagnostics for an explicitly selected SC2015 receiver interface."""

import argparse
from datetime import datetime, timezone
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
        ready &= command != 0xF8 or (reply[1] == 62 and reply[2] == 1)
        ready &= command != 0xFA or (reply[1] == 62 and reply[2] == 1)
        ready &= command != 0xFC or reply[1] != 0
        ready &= command != 0xFD or reply[1] != 0
        if reply[0] == command and ready:
            return reply
    raise OSError("GET_FEATURE did not return the requested command")


QUEUE_TOTALS = ["haptic_admitted_total", "haptic_full_total", "a2_a6_full_total", "control_retries_total",
                "haptic_waits_total", "haptic_timeouts_total", "max_fifo_age_us", "max_usb_age_us",
                "max_rx_latency_us", "max_process_gap_us"]
QUEUE_ADMISSION = ["admitted", "haptic_admitted", "haptic_full", "a2_full", "a3_full", "a4_full", "a5_full",
                   "a6_full", "other_full", "initial_retries", "count_retries", "commit_waits"]
QUEUE_PROGRESS = ["offline", "serialized", "retired", "cleared_fragments", "max_fifo_age_us", "max_lane_age_us",
                  "usb_admitted", "usb_processed", "usb_haptic_received", "usb_haptic_waits", "usb_haptic_timeouts", "usb_haptic_stale"]
QUEUE_KINDS = {1: "HapticFull", 2: "BestEffortFull", 3: "OtherFull", 4: "InitialRetry", 5: "CountRetry",
               6: "UsbWait", 7: "UsbTimeout", 8: "StaleUsb", 9: "Offline"}


class QueueSnapshotExpiredError(OSError):
    pass


def confirm_queue_support(device, diagnostic=None, link=None):
    identity = exchange(device, 0xA1)
    if (identity[1] != 34 or identity[16:36].split(b"\x00")[0].split(b"\xff")[0] != b"SC2015 XIAO"
            or any(c not in b"0123456789abcdefABCDEF" for c in identity[6:16])):
        raise OSError("Queue diagnostics require a confirmed SC2015 XIAO receiver")
    diagnostic = diagnostic if diagnostic is not None else exchange(device, 0xF0)
    address = bytearray.fromhex(identity[6:16].decode("ascii"))
    address[0] &= 0xF8
    if (diagnostic[1] != 62 or diagnostic[2] != 3 or diagnostic[33] >= 4 or diagnostic[34] != 4
            or diagnostic[59:64] != address):
        raise OSError("Queue diagnostics require matching XIAO identity and diagnostics protocol v3")
    link = link if link is not None else exchange(device, 0xF7)
    if link[1] != 62 or link[2] != 1 or link[3] < 11 or link[63] != link[3]:
        raise OSError("Queue diagnostics require validated F7 firmware revision 11 or newer")


def queue_page(device, page, snapshot=None):
    arguments = b"" if page == 0 else bytes([page]) + struct.pack("<I", snapshot["snapshot_id"])
    reply = exchange(device, 0xFC, arguments)
    if reply[:4] != bytes([0xFC, 62, 1, page]) or reply[4] < 11 or reply[5] > 2:
        raise OSError("Invalid FC page, schema, revision or status")
    if reply[5] == 2:
        raise QueueSnapshotExpiredError("FC snapshot expired; retry without another queue reader")
    if reply[5] == 1:
        raise OSError("FC rejected the page selector")
    header = {"version": reply[2], "firmware_revision": reply[4],
              "snapshot_id": struct.unpack_from("<I", reply, 6)[0],
              "snapshot_clock_us": struct.unpack_from("<I", reply, 10)[0]}
    if not header["snapshot_id"] or (snapshot and any(header[key] != snapshot[key] for key in header)):
        raise OSError("FC snapshot ID or MCU clock mismatch")
    if page == 0:
        if reply[14:16] != bytes([12, 31]) or reply[16] > 16 or reply[17] > 31 or reply[62] > 12 or reply[63] > 1:
            raise OSError("Invalid FC summary capacities or counts")
        return dict(header, rf_capacity=reply[14], usb_capacity=reply[15], event_count=reply[16],
                    usb_high_water=reply[17], counters=dict(zip(QUEUE_TOTALS, struct.unpack_from("<10I", reply, 18))),
                    haptic_wait_budget_us=struct.unpack_from("<I", reply, 58)[0],
                    rf_high_water=reply[62], clock_available=bool(reply[63]))
    if page <= 8:
        if reply[62] > snapshot["rf_capacity"] or reply[63] > snapshot["usb_capacity"]:
            raise OSError("Invalid FC slot queue state")
        return dict(zip(QUEUE_ADMISSION if page <= 4 else QUEUE_PROGRESS, struct.unpack_from("<12I", reply, 14)),
                    fifo_high_water=reply[62], pending_haptics=reply[63])
    if page <= 16:
        if reply[62] != min(2, snapshot["event_count"] - (page - 9) * 2):
            raise OSError("FC event row count mismatch")
        events = []
        for index in range(reply[62]):
            offset = 14 + 24 * index
            event = dict(zip(["id", "at_us", "generation", "age_us", "slot", "kind", "command", "length",
                              "needed", "fifo_count", "lane_flags", "usb_depth"], struct.unpack_from("<4I8B", reply, offset)))
            if (event["slot"] > 3 or event["kind"] not in QUEUE_KINDS or event["fifo_count"] > snapshot["rf_capacity"]
                    or event["lane_flags"] > 15 or event["usb_depth"] > snapshot["usb_capacity"]):
                raise OSError("Invalid FC event metadata")
            event["kind_name"] = QUEUE_KINDS[event["kind"]]
            events.append(event)
        return events
    if reply[62] > snapshot["usb_capacity"] or reply[63] != 4:
        raise OSError("Invalid FC USB depth or batch")
    return {"usb_depth": reply[62], "usb_batch": reply[63], "slots": [
        dict(zip(["usb_max_age_us", "usb_haptic_offline", "usb_haptic_reset_cancelled"],
                 struct.unpack_from("<3I", reply, 14 + 12 * slot))) for slot in range(4)]}


def read_queue_diagnostics(device, detail=False):
    """Read at most 18 pages per attempt; discard an expired read and retry once."""
    for attempt in range(2):
        try:
            snapshot = dict(queue_page(device, 0), detail=detail)
            if detail:
                snapshot["slots"] = []
                for page in range(1, 9):
                    data = queue_page(device, page, snapshot)
                    slot = (page - 1) % 4
                    if page <= 4:
                        snapshot["slots"].append(dict(data, slot=slot))
                    else:
                        if any(data[key] != snapshot["slots"][slot][key] for key in ("fifo_high_water", "pending_haptics")):
                            raise OSError("FC slot snapshot mismatch")
                        snapshot["slots"][slot].update(data)
                usb = queue_page(device, 17, snapshot)
                snapshot.update(usb_depth=usb["usb_depth"], usb_batch=usb["usb_batch"])
                for slot, data in zip(snapshot["slots"], usb["slots"]):
                    slot.update(data)
                snapshot["events"] = []
                for index in range((snapshot["event_count"] + 1) // 2):
                    snapshot["events"].extend(queue_page(device, 9 + index, snapshot))
            snapshot["captured_at"] = datetime.now(timezone.utc).isoformat()
            return snapshot
        except QueueSnapshotExpiredError:
            if attempt:
                raise


def parse_input_mode(reply, revision=None):
    mode = normalize(reply)
    if (mode[:3] != bytes([0xFD, 62, 1]) or mode[3] < 12 or (revision is not None and mode[3] != revision)
            or mode[4] or mode[5] > (4 if mode[3] >= 19 else 3 if mode[3] >= 18 else 2 if mode[3] >= 17 else 1)
            or mode[6] > (4 if mode[3] >= 19 else 3 if mode[3] >= 18 else 2 if mode[3] >= 17 else 1) or mode[7] not in (0, 1, 2, 3, 255)
            or mode[8] & ~(31 if mode[3] >= 16 else 7)):
        raise OSError("Invalid input-mode report")
    names = ["steam", "xbox", "ps5", "switch", "lizard"]
    result = {"firmware_revision": mode[3], "mode": names[mode[5]],
              "requested_mode": names[mode[6]],
              "owner_slot": None if mode[7] == 255 else mode[7], "reset_on_unplug": bool(mode[8] & 4),
              "switching": bool(mode[8] & 2), "persist_mode": bool(mode[8] & 8),
              "switch_save_failed": bool(mode[8] & 16), "xinput": None}
    if mode[3] >= 14:
        if mode[9] != 1 or mode[10] & ~15 or mode[11]:
            raise OSError("Invalid XInput diagnostics")
        names = ["generation", "opens", "resets", "os_queries", "in_submitted", "in_completed", "in_failed",
                 "out_completed", "out_failed", "control_requests"]
        result["xinput"] = dict(zip(names, struct.unpack_from("<10I", mode, 12)),
            version=mode[9], enabled=bool(mode[10] & 1), opened=bool(mode[10] & 2),
            mounted=bool(mode[10] & 4), suspended=bool(mode[10] & 8), last_setup=mode[52:60].hex(),
            control_rejected=struct.unpack_from("<I", mode, 60)[0])
    return result


def receiver_interfaces(hid):
    devices = []
    for product, first in ((0x1142, 1), (0x1143, 2)):
        for entry in hid.enumerate(0x28DE, product):
            page = entry.get("usage_page")
            vendor = page == 0xFF00 and entry.get("usage", 1) == 1
            unknown_usage = page in (None, 0) and first <= entry.get("interface_number", -1) < first + 4
            if vendor or unknown_usage:
                devices.append(entry)
    return devices


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["list", "status", "queue", "pair", "unpair-mode", "commit", "forget", "capture"],
                        help="status reads a queue summary on revision 11+; queue reads the full frozen snapshot")
    parser.add_argument("--path", help="Exact vendor HID path printed by list (Steam IF1..4, XInput IF2..5)")
    parser.add_argument("--seconds", type=float, default=10)
    args = parser.parse_args()
    try:
        import hid
    except ImportError:
        raise SystemExit("Install the host helper with: python -m pip install hidapi")
    devices = receiver_interfaces(hid)
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
                if link[1] != 62 or link[2] != 1 or link[3] < 3 or link[63] != link[3]:
                    raise OSError("Invalid F7 diagnostics revision")
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
                if link[3] >= 4:
                    reconnect = exchange(device, 0xF8)
                    names = ["lookup_hits", "lookup_misses", "restore_requests", "restore_accepted",
                             "restore_mic_failures", "new_pairs", "key_updates", "commit_requests",
                             "commit_deferred", "commit_successes", "commit_failures",
                             "unsaved_disconnects", "restore_fallbacks"]
                    result["reconnect"] = dict(zip(names, struct.unpack_from("<13I", reconnect, 8)))
                    result["reconnect"].update(bond_count=reconnect[4], bootstrap_records=reconnect[5],
                                              lookup_valid=bool(reconnect[6]), last_outcome=reconnect[7],
                                              pending_mask=reconnect[60], key_not_ready_mask=reconnect[61],
                                               commit_requested_mask=reconnect[62])
                if link[3] >= 6:
                    host = exchange(device, 0xFA)
                    names = ["sets", "query_busy", "query_ready", "gets", "unpublished_gets", "invalid_sets", "command_drops"]
                    result["host_link"] = dict(zip(names, struct.unpack_from("<7I", host, 12)))
                    result["host_link"].update(last_set_slot=host[4], last_set_command=hex(host[5]),
                                               last_set_size=host[6], last_get_slot=host[7],
                                               last_get_command=hex(host[8]), last_get_payload_length=host[9],
                                               connected_mask=host[60], pending_sets_mask=host[61],
                                                slots=[dict(zip(["query_command", "query_size", "query_status", "reply_command", "reply_length"],
                                                                host[40 + 5 * slot:45 + 5 * slot])) for slot in range(4)])
                    result["host_link"]["last_rejected"] = (
                        {"slot": host[62] >> 6, "command": hex(host[10]),
                         "declared_length": host[11], "reason": host[62] & 63}
                        if host[3] >= 9 and host[62] & 63 else None)
                if link[3] >= 10:
                    transport = exchange(device, 0xFB)
                    names = ["watchdog_disconnects", "host_disconnects", "replaced_disconnects",
                             "crypto_disconnects", "erase_disconnects", "ack_deferred", "stale_session_sets"]
                    result["transport_link"] = {
                        "counters": dict(zip(names, struct.unpack_from("<7I", transport, 4))),
                        "connected_mask": transport[60],
                        "slots": [dict(zip(["last_disconnect_reason", "watchdog", "queued", "lane_phase_flags",
                                            "rx_counter_low8", "tx_counter_low8"],
                                           transport[32 + 7 * slot:38 + 7 * slot])) for slot in range(4)]}
                if link[3] >= 11:
                    confirm_queue_support(device, reply, link)
                    result["queue_diagnostics"] = read_queue_diagnostics(device)
                if link[3] >= 12:
                    result["input_mode"] = parse_input_mode(exchange(device, 0xFD), link[3])
            result["slot_state"] = exchange(device, 0xB4)[2]
            print(json.dumps(result, indent=2))
        elif args.action == "queue":
            confirm_queue_support(device)
            print(json.dumps(read_queue_diagnostics(device, detail=True), indent=2))
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
