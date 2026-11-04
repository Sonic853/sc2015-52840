"""Strict application-only Intel HEX and nRF52840 UF2 handling."""

import hashlib
from pathlib import Path
import struct

APP_START = 0x27000
STORE_START = 0xE9000
FAMILY = 0xADA52840


def read_hex(path, app_start=APP_START):
    memory, base, eof = {}, 0, False
    for line in Path(path).read_text().splitlines():
        if not line:
            continue
        if eof or not line.startswith(":"):
            raise ValueError("Data after HEX EOF or invalid record")
        raw = bytes.fromhex(line[1:])
        if len(raw) < 5 or len(raw) != raw[0] + 5 or sum(raw) & 255:
            raise ValueError("Invalid HEX length or checksum")
        count, kind = raw[0], raw[3]
        offset = int.from_bytes(raw[1:3], "big")
        data = raw[4:-1]
        if kind == 0:
            for i, value in enumerate(data):
                address = base + offset + i
                if address in memory:
                    raise ValueError("Overlapping HEX records")
                memory[address] = value
        elif kind == 1 and count == 0:
            eof = True
        elif kind == 4 and count == 2 and offset == 0:
            base = int.from_bytes(data, "big") << 16
        elif kind == 2 and count == 2 and offset == 0:
            base = int.from_bytes(data, "big") << 4
        elif kind in (3, 5) and count == 4 and offset == 0:
            pass
        else:
            raise ValueError(f"Unsupported HEX record {kind}")
    if not eof:
        raise ValueError("Missing HEX EOF")
    validate_memory(memory, app_start)
    return memory


def validate_memory(memory, app_start=APP_START):
    if app_start not in (0x26000, 0x27000):
        raise ValueError("Unsupported application layout")
    if not memory or min(memory) != app_start or max(memory) >= STORE_START:
        raise ValueError("Image outside selected application layout or overlaps persistent storage")
    try:
        sp, reset = struct.unpack("<II", bytes(memory[app_start + i] for i in range(8)))
    except KeyError as error:
        raise ValueError("Missing vector table") from error
    if not (0x20006000 <= sp <= 0x20040000 and sp % 8 == 0 and reset & 1
            and app_start <= (reset & ~1) <= max(memory)):
        raise ValueError("Invalid application vector table")
    return sp, reset


def write_uf2(memory, path, app_start=APP_START):
    validate_memory(memory, app_start)
    end = (max(memory) + 256) & ~255
    count = (end - app_start) // 256
    output = bytearray()
    for index, address in enumerate(range(app_start, end, 256)):
        block = bytearray(512)
        struct.pack_into("<8I", block, 0, 0x0A324655, 0x9E5D5157,
                         0x2000, address, 256, index, count, FAMILY)
        block[32:288] = bytes(memory.get(address + i, 255) for i in range(256))
        struct.pack_into("<I", block, 508, 0x0AB16F30)
        output.extend(block)
    Path(path).write_bytes(output)
    return validate_uf2(path, app_start)


def validate_uf2(path, app_start=APP_START):
    data = Path(path).read_bytes()
    if not data or len(data) % 512:
        raise ValueError("Invalid UF2 length")
    memory, seen = {}, set()
    count = len(data) // 512
    for offset in range(0, len(data), 512):
        block = data[offset:offset + 512]
        m0, m1, flags, address, size, index, total, family = struct.unpack_from("<8I", block)
        if ((m0, m1) != (0x0A324655, 0x9E5D5157)
                or flags != 0x2000 or family != FAMILY or size != 256
                or total != count or index >= count or index in seen
                 or address % 256 or address < app_start
                or address + size > STORE_START
                or struct.unpack_from("<I", block, 508)[0] != 0x0AB16F30):
            raise ValueError("Invalid UF2 block")
        seen.add(index)
        for i, value in enumerate(block[32:288]):
            if address + i in memory:
                raise ValueError("Overlapping UF2 blocks")
            memory[address + i] = value
    if len(memory) != max(memory) + 1 - min(memory):
        raise ValueError("Missing UF2 application blocks")
    sp, reset = validate_memory(memory, app_start)
    return {"application_start": hex(min(memory)), "application_end": hex(max(memory) + 1),
            "stack_pointer": hex(sp), "reset_vector": hex(reset), "uf2_blocks": count,
            "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
