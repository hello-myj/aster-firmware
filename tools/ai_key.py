"""Manage AI credentials through the device's local serial console."""
import argparse
import getpass
import os
import re
import struct
import time
from pathlib import Path

import serial
from serial.tools import list_ports


def wait_for(port, message):
    end = time.monotonic() + 10
    while time.monotonic() < end:
        line = port.readline().strip()
        if line == message:
            return
        if line == b"AIKEY ERROR":
            raise SystemExit("Device rejected the credential request.")
    raise SystemExit("Timed out waiting for the AI firmware. Check port and firmware configuration.")


def wait_for_console(port):
    deadline = time.monotonic() + 16
    while time.monotonic() < deadline:
        port.write(b"\x03\n")
        time.sleep(.15)
        port.write(b"ASTERPING\n")
        until = time.monotonic() + 1.3
        while time.monotonic() < until:
            line = port.readline()
            if b"PONG " in line and b"product=aster " in line:
                return
    raise SystemExit("No Aster serial console on this port.")


def speech_payload(xfyun, volc, speaker):
    def fields(path, names):
        text = path.read_text(encoding="utf-8-sig")
        for name in names:
            match = re.search(rf"^\s*{name}(?:\s*[:：=]\s*|\s+)([A-Za-z0-9_-]+)\s*$", text, re.I | re.M)
            if not match:
                raise SystemExit(f"Missing or invalid credential field {name}.")
            yield match[1]

    values = [*fields(xfyun, ("appid", "apikey", "apisecret")),
              *fields(volc, (r"api\s*key",)), speaker]
    caps = (24, 65, 65, 96, 80)
    if any(not re.fullmatch(r"[A-Za-z0-9_-]+", value) or len(value) >= cap
           for value, cap in zip(values, caps)):
        raise SystemExit("Invalid speech credential or speaker ID.")
    return struct.pack("<24s65s65s96s80s", *(value.encode("ascii") for value in values))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--file", type=Path)
    source.add_argument("--clear", action="store_true")
    source.add_argument("--clear-speech", action="store_true")
    source.add_argument("--xfyun", type=Path)
    parser.add_argument("--volc", type=Path)
    parser.add_argument("--speaker", default="zh_female_vv_uranus_bigtts")
    args = parser.parse_args()
    if bool(args.xfyun) != bool(args.volc):
        parser.error("--xfyun and --volc must be provided together")
    if args.xfyun:
        payload = speech_payload(args.xfyun, args.volc, args.speaker)
        command = "speech"
    elif args.clear_speech:
        payload = b""
        command = "speech-clear"
    else:
        raw = "" if args.clear else (args.file.read_text(encoding="utf-8-sig").strip()
                                     if args.file else getpass.getpass("DeepSeek API key: "))
        if not args.clear and not (12 <= len(raw) < 160 and all(33 <= ord(c) <= 126 for c in raw)):
            raise SystemExit("Invalid credential file or input.")
        payload = raw.encode("ascii")
        command = str(len(payload))
    bridge = next((p for p in list_ports.comports() if p.device == args.port), None)
    port = serial.Serial(port=None, baudrate=115200, timeout=.25, write_timeout=5)
    port.dtr = port.rts = bool(bridge and (bridge.vid == 0x1a86 or
                                           (bridge.vid, bridge.pid) == (0x303a, 0x1001)))
    port.port = args.port
    with port:
        if os.name == "posix":
            import termios
            attrs = termios.tcgetattr(port.fileno())
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(port.fileno(), termios.TCSANOW, attrs)
        port.reset_input_buffer()
        wait_for_console(port)
        port.write(f"ASTERAIKEY {command}\n".encode("ascii"))
        wait_for(port, b"AIKEY READY")
        if payload:
            port.write(payload)
        wait_for(port, b"AIKEY OK")
    print("Credential cleared." if args.clear or args.clear_speech else "Credential configured.")


if __name__ == "__main__":
    main()
