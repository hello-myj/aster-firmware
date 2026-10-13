#!/usr/bin/env python3
"""Check exported capabilities/CPU/display against an actual Zephyr build."""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FEATURES = {"display": "DISPLAY", "touch": "TOUCH", "mouse": "MOUSE", "keyboard": "KEYS",
            "camera": "CAMERA", "audio-in": "AUDIO_IN", "audio-out": "AUDIO_OUT",
            "wifi": "WIFI", "bluetooth": "BT", "accelerometer": "ACCEL", "gyroscope": "GYRO",
            "filesystem": "FILESYSTEM", "storage.app": "FILESYSTEM", "rtc": "RTC",
            "psram": "PSRAM", "framebuffer": "FRAMEBUFFER"}


def check(target_id, build=None):
    record = json.loads((ROOT / "hardware" / f"{target_id}.json").read_text())
    target = record["public"]
    build = build or ROOT / "build/firmware"
    text = (build / "zephyr/.config").read_text()
    config = dict(re.findall(r'^CONFIG_(\w+)=(.*)$', text, re.M))
    selected_symbol = record["build"]["kconfig"]
    if config.get(selected_symbol) != "y":
        raise ValueError(f"hardware identity drift: expected CONFIG_{selected_symbol}=y for {target_id}")
    actual = {name for name, key in FEATURES.items() if config.get("ASTER_HAS_" + key) == "y"}
    if not actual <= set(target["capabilities"]):
        raise ValueError(f"capability drift: configured={sorted(actual)} published={target['capabilities']}")
    platform_ids = {"xtensa": "xtensa", "riscv32": "riscv", "riscv64": "riscv",
                    "arm": "arm", "x86_64": "x86"}
    if config.get("ASTER_TARGET_ID") != json.dumps(platform_ids[target["cpu"]]):
        raise ValueError("firmware platform id differs from CPU arch")
    cpu_symbols = {"xtensa": "XTENSA", "riscv32": "RISCV", "riscv64": "RISCV",
                   "arm": "ARM", "x86_64": "X86"}
    if config.get("ASTER_CPU_ARCH_" + cpu_symbols[target["cpu"]]) != "y":
        raise ValueError("CPU differs from published target")
    aot_ids = {"xtensa": 1, "riscv32": 2, "thumbv7": 3, "thumbv8": 4, "x86_64": 5}
    if int(config["ASTER_AOT_TARGET_ID"]) != aot_ids[target["aot_target"]]:
        raise ValueError("reference target AOT ID mismatch")
    if "display" in actual:
        # Read the selected display node, not arbitrary width values from other peripherals.
        dts = (build / "zephyr/zephyr.dts").read_text()
        chosen = re.search(r'zephyr,display\s*=\s*&([^;]+);', dts)
        if not chosen:
            raise ValueError("missing chosen display")
        node = re.search(r'\b' + re.escape(chosen[1].strip()) + r':\s*[^\{]+\{([^}]+)', dts)
        if not node:
            raise ValueError("cannot resolve chosen display node")
        for edge in ("width", "height"):
            value = re.search(r'\b' + edge + r'\s*=\s*<\s*(0x[0-9a-fA-F]+|\d+)\s*>', node[1])
            if not value or int(value[1], 0) != target["display"][edge]:
                raise ValueError("display geometry differs from published target: " + edge)
    print("PASS target contract:", target_id)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    from firmware import hardware
    check(hardware()["public"]["id"], args.build_dir)
