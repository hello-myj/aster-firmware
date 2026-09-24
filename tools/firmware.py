#!/usr/bin/env python3
"""Configure hardware in menuconfig, then build/flash one firmware image."""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party/kconfig"))
CONFIG = ROOT / "config/aster.conf"
BUILD = ROOT / "build/firmware"


def load_kconfig():
    import kconfiglib
    config = kconfiglib.Kconfig(str(ROOT / "config/Kconfig"))
    config.load_config(str(CONFIG if CONFIG.exists() else ROOT / "config/default.conf"))
    return config


def hardware():
    text = (CONFIG if CONFIG.exists() else ROOT / "config/default.conf").read_text()
    matches = []
    for path in (ROOT / "hardware").glob("*.json"):
        item = json.loads(path.read_text())
        if re.search(r"^CONFIG_" + item["build"]["kconfig"] + "=y$", text, re.M):
            matches.append(item)
    if len(matches) != 1:
        raise SystemExit("Select one hardware model with: python tools/firmware.py menuconfig")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["menuconfig", "build", "flash", "config"])
    args = parser.parse_args()
    if args.command == "menuconfig":
        import menuconfig
        config = load_kconfig()
        os.environ["KCONFIG_CONFIG"] = str(CONFIG)
        menuconfig.menuconfig(config)
        return 0
    selected = hardware()
    if args.command == "config":
        print(selected["public"]["name"])
        print((CONFIG if CONFIG.exists() else ROOT / "config/default.conf").read_text())
        return 0
    if args.command == "flash":
        return subprocess.call(["west", "flash", "-d", str(BUILD)], cwd=ROOT)
    config = load_kconfig()
    config.write_config(str(CONFIG))
    cmd = ["west", "build", "-d", str(BUILD), "-b", selected["build"]["board"],
           "-p", "auto", str(ROOT), "--", f"-DPython3_EXECUTABLE={sys.executable}"]
    result = subprocess.call(cmd, cwd=ROOT)
    if result == 0:
        from check_target import check
        check(selected["public"]["id"], BUILD)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
