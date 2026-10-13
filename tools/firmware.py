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


def load_kconfig(path=None):
    import kconfiglib
    config = kconfiglib.Kconfig(str(ROOT / "config/Kconfig"))
    config.load_config(str(path or (CONFIG if CONFIG.exists() else ROOT / "config/default.conf")))
    return config


def hardware(path=None):
    text = (path or (CONFIG if CONFIG.exists() else ROOT / "config/default.conf")).read_text()
    matches = []
    for path in (ROOT / "hardware").glob("*.json"):
        item = json.loads(path.read_text())
        if re.search(r"^CONFIG_" + item["build"]["kconfig"] + "=y$", text, re.M):
            matches.append(item)
    if len(matches) != 1:
        raise SystemExit("Select one hardware model with: python tools/firmware.py menuconfig")
    return matches[0]


def check_build_fragments(build_dir, selected_config):
    """Zephyr appends build-root *.conf after CONF_FILE; reject stray profiles."""
    allowed = {Path(selected_config).resolve(), (Path(build_dir) / "aster-services.conf").resolve()}
    unexpected = [p for p in Path(build_dir).glob("*.conf") if p.resolve() not in allowed]
    if unexpected:
        raise ValueError("Unexpected build-root Kconfig fragments (Zephyr auto-merges them): " +
                         ", ".join(str(p) for p in sorted(unexpected)) +
                         ". Move them outside the build directory or select the intended file with --config.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["menuconfig", "build", "flash", "config"])
    parser.add_argument("--config", type=Path, help="Use a separate hardware selection file")
    parser.add_argument("--build-dir", type=Path, help="Use a separate build directory")
    args = parser.parse_args()
    selected_config = args.config.resolve() if args.config else CONFIG
    build_dir = args.build_dir.resolve() if args.build_dir else BUILD
    if args.command == "menuconfig":
        import menuconfig
        config = load_kconfig(args.config)
        os.environ["KCONFIG_CONFIG"] = str(selected_config)
        menuconfig.menuconfig(config)
        return 0
    if args.command in ("build", "flash"):
        check_build_fragments(build_dir, selected_config)
    selected = hardware(args.config)
    if args.command == "config":
        print(selected["public"]["name"])
        print((args.config or (CONFIG if CONFIG.exists() else ROOT / "config/default.conf")).read_text())
        return 0
    if args.command == "flash":
        from check_target import check
        check(selected["public"]["id"], build_dir)
        return subprocess.call(["west", "flash", "-d", str(build_dir)], cwd=ROOT)
    config = load_kconfig(args.config)
    if not args.config:
        config.write_config(str(CONFIG))
    cmd = ["west", "build", "-d", str(build_dir), "-b", selected["build"]["board"],
           "-p", "auto", str(ROOT), "--", f"-DPython3_EXECUTABLE={sys.executable}",
           f"-DASTER_USER_CONFIG={selected_config}"]
    result = subprocess.call(cmd, cwd=ROOT)
    if result == 0:
        from check_target import check
        check(selected["public"]["id"], build_dir)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
