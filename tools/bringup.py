#!/usr/bin/env python3
"""Build the experimental IDF port separately from supported Zephyr firmware."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", choices=["osptek-p4"])
    parser.add_argument("command", choices=["build", "menuconfig", "flash", "monitor"])
    parser.add_argument("--port", help="Serial device (flash/monitor only)")
    parser.add_argument("--revision", choices=["v3", "pre3"], default="v3",
                        help="P4 silicon family; default v3 per current OSPTEK core documentation")
    args = parser.parse_args()
    if not os.environ.get("IDF_PATH"):
        parser.error("Activate ESP-IDF 6.0 first: source /path/to/esp-idf/export.sh")
    idf = shutil.which("idf.py")
    if not idf:
        parser.error("idf.py is missing from PATH; activate ESP-IDF")
    project = ROOT / "boards/osptek_p4/idf"
    build = ROOT / ("build/osptek-p4" if args.revision == "v3" else "build/osptek-p4-pre3")
    defaults = str(project / "sdkconfig.defaults") + ";" + str(project / ("sdkconfig." + args.revision))
    command = [sys.executable, idf, "-C", str(project), "-B", str(build),
               "-DSDKCONFIG=" + str(build / "sdkconfig"),
               "-DSDKCONFIG_DEFAULTS=" + defaults]
    if args.port:
        if args.command not in ("flash", "monitor"):
            parser.error("--port is only valid for flash or monitor")
        command += ["-p", args.port]
    command.append(args.command)
    return subprocess.call(command, cwd=ROOT)


if __name__ == "__main__":
    raise SystemExit(main())
