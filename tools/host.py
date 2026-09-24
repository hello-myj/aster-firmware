#!/usr/bin/env python3
"""Build the developer ABI test runner, not a hardware firmware variant."""
import subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
if __name__ == "__main__":
    subprocess.run(["cmake", "-S", str(ROOT / "systems/posix"), "-B", str(ROOT / "build/host"), "-G", "Ninja"], check=True)
    subprocess.run(["cmake", "--build", str(ROOT / "build/host"), "--parallel", "4"], check=True)
