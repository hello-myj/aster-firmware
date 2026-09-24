#!/usr/bin/env python3
"""Create the firmware-owned SD-card resource directory."""
import argparse
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "dist/sdcard")
    args = parser.parse_args()
    source = ROOT / "resources/fonts"
    manifest = json.loads((source / "faces.json").read_text())
    for face in manifest["faces"]:
        path = source / face["file"]
        if path.stat().st_size != face["bytes"]:
            raise ValueError("font binary does not match published metadata")
    destination = args.output / "aster/fonts"
    destination.mkdir(parents=True, exist_ok=True)
    for face in manifest["faces"]:
        shutil.copy2(source / face["file"], destination / face["file"])
    print(f"Copy contents of {args.output} to the device SD-card root.")


if __name__ == "__main__":
    main()
