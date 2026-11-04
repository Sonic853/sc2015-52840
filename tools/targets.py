"""Pinned board profiles; an image must be linked and validated for its target."""

TARGETS = {
    "xiao": {
        "label": "XIAO nRF52840",
        "fqbn": "Seeeduino:nrf52:xiaonRF52840",
        "package": "Seeeduino", "core": "1.1.13",
        "index": "https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json",
        "softdevice": "7.3.0", "linker": "nrf52840_s140_v7.ld",
        "application_start": 0x27000, "define": "SC2015_BOARD_XIAO",
        "asset_board": "XIAO-nRF52840",
        "pins": "RGB LEDs active-low; pairing button D0/P0.02 to GND",
    },
    "promicro": {
        "label": "ProMicro nRF52840 (SuperMini)",
        "fqbn": "adafruit:nrf52:feather52840",
        "package": "adafruit", "core": "1.7.0",
        "index": "https://adafruit.github.io/arduino-board-index/package_adafruit_index.json",
        "softdevice": "6.1.1", "linker": "nrf52840_s140_v6.ld",
        "application_start": 0x26000, "define": "SC2015_BOARD_PROMICRO",
        "asset_board": "ProMicro-nRF52840",
        "pins": "SuperMini user LED P0.15 active-high; external pairing button P0.02 to GND",
    },
}


def uf2_name(target):
    return f"SCReceiver-{target}-s140-{TARGETS[target]['softdevice']}.uf2"
