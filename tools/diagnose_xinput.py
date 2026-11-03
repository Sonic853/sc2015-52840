"""Read Windows PnP/XInput state; optionally query one explicitly selected receiver HID."""

import argparse
import ctypes
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import receiverctl


# Literal script only: no user-controlled PowerShell expression or write cmdlet.
PNP_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$keys = @('DEVPKEY_Device_HardwareIds', 'DEVPKEY_Device_CompatibleIds',
  'DEVPKEY_Device_Service', 'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_DriverVersion',
  'DEVPKEY_Device_ProblemCode', 'DEVPKEY_Device_Parent', 'DEVPKEY_Device_ContainerId')
$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
  $_.InstanceId -match '^(USB|HID)\\(VID_28DE&PID_114[23]|VID_045E&PID_028E)(&|\\)'
} | ForEach-Object {
  $device = $_; $properties = @{}; $errors = @{}
  foreach ($key in $keys) {
    try {
      $property = Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName $key -ErrorAction Stop
      if ($property.Type -eq 'Empty') { $properties[$key] = $null }
      else { $properties[$key] = $property.Data }
    } catch { $errors[$key] = $_.Exception.Message }
  }
  [PSCustomObject]@{ instance_id = $device.InstanceId; status = $device.Status;
    class = $device.Class; name = $device.FriendlyName; properties = $properties; errors = $errors }
})
ConvertTo-Json -Depth 8 -InputObject ([PSCustomObject]@{ devices = $devices })
"""


class Gamepad(ctypes.Structure):
    _fields_ = [("buttons", ctypes.c_uint16), ("left_trigger", ctypes.c_uint8), ("right_trigger", ctypes.c_uint8),
                ("left_x", ctypes.c_int16), ("left_y", ctypes.c_int16),
                ("right_x", ctypes.c_int16), ("right_y", ctypes.c_int16)]


class State(ctypes.Structure):
    _fields_ = [("packet_number", ctypes.c_uint32), ("gamepad", Gamepad)]


class Vibration(ctypes.Structure):
    _fields_ = [("left_motor", ctypes.c_uint16), ("right_motor", ctypes.c_uint16)]


class Capabilities(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint8), ("subtype", ctypes.c_uint8), ("flags", ctypes.c_uint16),
                ("gamepad", Gamepad), ("vibration", Vibration)]


def read_pnp():
    executable = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    result = subprocess.run([str(executable), "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", PNP_SCRIPT],
                            capture_output=True, timeout=60)
    if result.returncode:
        raise OSError(result.stderr.decode("utf-8", errors="replace").strip())
    return json.loads(result.stdout.decode("utf-8-sig"))


def load_xinput():
    errors = {}
    for name in ("xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"):
        try:
            library = ctypes.WinDLL(name, winmode=0x00000800)  # LOAD_LIBRARY_SEARCH_SYSTEM32
            library.XInputGetState.argtypes = [ctypes.c_uint32, ctypes.POINTER(State)]
            library.XInputGetState.restype = ctypes.c_uint32
            library.XInputGetCapabilities.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(Capabilities)]
            library.XInputGetCapabilities.restype = ctypes.c_uint32
            library._guide_state = None
            library._guide_error = "Extended Guide state is unavailable in this DLL"
            if name in ("xinput1_4.dll", "xinput1_3.dll"):
                try:
                    # Desktop XInput's undocumented GetStateEx export includes
                    # Guide bit 0x0400, which ordinary GetState can mask out.
                    extended = library[100]
                    extended.argtypes = [ctypes.c_uint32, ctypes.POINTER(State)]
                    extended.restype = ctypes.c_uint32
                    library._guide_state = extended
                    library._guide_error = None
                except (OSError, AttributeError) as error:
                    library._guide_error = str(error)
            return library, name, errors
        except (OSError, AttributeError) as error:
            errors[name] = str(error)
    return None, None, errors


def sample_xinput(library):
    slots = []
    for index in range(4):
        state, caps = State(), Capabilities()
        code = library.XInputGetState(index, ctypes.byref(state))
        row = {"index": index, "state_result": code, "connected": True if code == 0 else False if code == 1167 else None,
               "device_attribution": "unknown"}
        if code == 0:
            row.update(packet_number=state.packet_number,
                       gamepad={name: getattr(state.gamepad, name) for name, _ in Gamepad._fields_})
        cap_code = library.XInputGetCapabilities(index, 0, ctypes.byref(caps))
        row["capabilities_result"] = cap_code
        if cap_code == 0:
            row["capabilities"] = {"type": caps.type, "subtype": caps.subtype, "flags": caps.flags}
        extended = vars(library).get("_guide_state")
        if extended is not None:
            guide_state = State()
            guide_code = extended(index, ctypes.byref(guide_state))
            row["guide"] = {"source": "XInputGetStateEx ordinal 100 (undocumented)", "result": guide_code,
                            "pressed": bool(guide_state.gamepad.buttons & 0x0400) if guide_code == 0 else None}
        else:
            row["guide"] = {"source": None, "pressed": None,
                            "unavailable": vars(library).get("_guide_error", "extended API not probed")}
        slots.append(row)
    return slots


def open_receiver(path):
    try:
        import hid
    except ImportError:
        local = Path(__file__).resolve().parents[1] / ".tools/hid-probe"
        if local.is_dir():
            sys.path.insert(0, str(local))
        try:
            import hid
        except ImportError as error:
            raise OSError("Optional --path requires hidapi; PnP/XInput capture needs no third-party package") from error
    encoded = path.encode("utf-8")
    if not any(entry["path"] == encoded for entry in receiverctl.receiver_interfaces(hid)):
        raise OSError("Selected vendor HID interface is not currently present")
    device = hid.device()
    try:
        device.open_path(encoded)
        # Verify shipped identity/address before using any local extension.
        receiverctl.confirm_queue_support(device)
        link = receiverctl.exchange(device, 0xF7)
        if link[3] < 12:
            raise OSError("Input-mode diagnostics require revision 12 or newer")
        return device, link[3]
    except Exception:
        device.close()
        raise


def capture(samples, interval, path=None):
    report = {"format": "sc2015-windows-xinput-diagnostics", "version": 1,
              "captured_at": datetime.now(timezone.utc).isoformat(), "platform": sys.platform,
              "limitations": ["XInput indices cover all controllers and are not mapped to a receiver by this tool.",
                              "PnP enumeration and samples are sequential, not an atomic system snapshot."],
              "errors": {}, "samples": []}
    if sys.platform != "win32":
        report["errors"]["platform"] = "Windows is required"
        return report
    try:
        report["pnp"] = read_pnp()
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report["errors"]["pnp"] = str(error)
    library, name, errors = load_xinput()
    report["xinput_library"] = {"name": name, "load_errors": errors}
    if library is None:
        report["errors"]["xinput"] = "No system XInput DLL could be loaded"
    device, revision = None, None
    if path:
        try:
            device, revision = open_receiver(path)
            report["receiver_path"] = path
        except (OSError, ValueError) as error:
            report["errors"]["receiver"] = str(error)
    start = time.monotonic()
    try:
        for index in range(samples):
            if index:
                time.sleep(interval)
            row = {"elapsed_ms": round((time.monotonic() - start) * 1000, 3)}
            if library is not None:
                row["xinput"] = sample_xinput(library)
            if device is not None:
                try:
                    row["receiver"] = receiverctl.parse_input_mode(receiverctl.exchange(device, 0xFD), revision)
                except (OSError, ValueError) as error:
                    row["receiver_error"] = str(error)
                    report["errors"]["receiver"] = str(error)
                    device.close()
                    device = None  # Do not keep retrying a removed or contested device.
            report["samples"].append(row)
    finally:
        if device is not None:
            device.close()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", help="Optional exact vendor HID path from receiverctl.py list; close other Feature readers first")
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--output", type=Path, help="New JSON file; existing files are refused (otherwise print JSON)")
    args = parser.parse_args()
    if not 1 <= args.samples <= 120 or not 0.05 <= args.interval <= 5:
        parser.error("Use 1..120 samples and a 0.05..5 second interval")
    if args.output and (not args.output.parent.is_dir() or args.output.exists()):
        parser.error("Output parent must exist and the output file must be absent")
    result = capture(args.samples, args.interval, args.path)
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        with args.output.open("x", encoding="utf-8") as output:
            output.write(text)
        print(args.output)
    else:
        print(text)
    return 1 if result["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
