# /// script
# requires-python = ">=3.10"
# dependencies = ["websockets>=15,<17", "dnspython>=2.6,<3"]
# ///
"""Time the same DeepSeek chat and first spoken clause as 'aster ui ai-bench'.

Use --key-file PATH or DEEPSEEK_API_KEY. Each trial makes a real chat request
and, unless --chat-only is set, synthesizes its first clause. No microphone
or sound device is needed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import http.client
import json
import os
from pathlib import Path
import socket
import ssl
import statistics
import struct
import sys
import threading
import time

from websockets.sync.client import connect

HOST = "api.deepseek.com"
MODEL = "deepseek-flash"
SYSTEM = "你是 Aster 语音助手小鲸。先用一句话直接回答；除非用户要求详细解释，一到两句话即可。用用户的语言，不用 Markdown。"
STORY = "请用五个完整的句子，用中文讲一个小鲸寻找月亮的小故事。"


def dial(args, record, prefix):
    start = time.perf_counter()
    if args.dns_server:
        import dns.resolver
        resolver = dns.resolver.Resolver(configure=False)
        resolver.nameservers = [args.dns_server]
        addresses = [r.address for r in resolver.resolve(HOST, "A", source=args.bind_ip, lifetime=6)]
    else:
        addresses = [a[4][0] for a in socket.getaddrinfo(HOST, 443, socket.AF_INET, socket.SOCK_STREAM)]
    resolved = time.perf_counter()
    record[prefix + "_dns_ms"] = round(1000 * (resolved - start))
    for address in dict.fromkeys(addresses):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.settimeout(20)
            if args.interface_index:
                sock.setsockopt(socket.IPPROTO_IP, getattr(socket, "IP_UNICAST_IF", 31),
                                struct.pack("!I", args.interface_index))
            if args.bind_ip:
                sock.bind((args.bind_ip, 0))
            sock.connect((address, 443))
        except OSError:
            sock.close()
            continue
        record[prefix + "_tcp_ms"] = round(1000 * (time.perf_counter() - resolved))
        record[prefix + "_peer"] = address
        record[prefix + "_local"] = sock.getsockname()[0]
        return sock
    raise ConnectionError(f"Could not connect to {HOST} on the selected IPv4 route")


class TimedHTTPS(http.client.HTTPSConnection):
    def __init__(self, args, record, context):
        super().__init__(HOST, timeout=20, context=context)
        self.args = args
        self.record = record

    def connect(self):
        raw = dial(self.args, self.record, "chat")
        start = time.perf_counter()
        try:
            self.sock = self._context.wrap_socket(raw, server_hostname=HOST)
        except BaseException:
            raw.close()
            raise
        self.record["chat_tls_ms"] = round(1000 * (time.perf_counter() - start))
        self.record["chat_new_connection"] = True


def first_clause(text):
    byte_offset = 0
    for i, char in enumerate(text):
        if (char in "。！？!?" or (byte_offset >= 24 and char in "，；：") or
                (byte_offset >= 12 and char == "." and text[i+1:i+2] == " ") or
                (byte_offset >= 32 and char in ",;\n")):
            return i + 1
        byte_offset += len(char.encode())
    return 0


def one_trial(args, key, warm):
    record = {"mode": "warm" if warm else "cold", "model": MODEL, "tts": not args.chat_only}
    context = ssl.create_default_context()
    context.set_alpn_protocols(["http/1.1"])
    auth = {"Authorization": "Bearer " + key}
    chat = TimedHTTPS(args, record, context)
    ready = threading.Event()
    voice = {}
    body = json.dumps({"model": MODEL, "stream": True, "max_tokens": 256,
                       "thinking": {"type": "disabled"}, "messages": [
                           {"role": "system", "content": SYSTEM},
                           {"role": "user", "content": args.prompt}]}, ensure_ascii=False).encode()
    if warm:
        try:
            chat.request("GET", "/models", headers=auth)
            response = chat.getresponse()
            response.read()
            if response.status != 200 or chat.sock is None:
                raise RuntimeError(f"Unable to warm HTTP connection (HTTP {response.status})")
        except BaseException:
            chat.close()
            raise
        for k in ("chat_dns_ms", "chat_tcp_ms", "chat_tls_ms"):
            record[k] = 0
    record["chat_new_connection"] = False
    origin = time.perf_counter()

    def stamp(name):
        record.setdefault(name, round(1000 * (time.perf_counter() - origin)))

    def get_audio():
        raw = dial(args, record, "tts")
        try:
            with connect("wss://" + HOST + "/v1/tools/tts?format=pcm&voice_id=stella",
                         sock=raw, ssl=context, additional_headers=auth, proxy=None,
                         compression=None, open_timeout=12, close_timeout=2) as ws:
                voice["socket"] = ws
                stamp("tts_connected")
                ready.set()
                audio_bytes = 0
                while True:
                    msg = ws.recv(timeout=25)
                    if isinstance(msg, bytes):
                        if msg:
                            stamp("tts_first_pcm")
                        audio_bytes += len(msg)
                        if audio_bytes >= 16384:
                            stamp("tts_prebuffered")
                    else:
                        event = json.loads(msg)
                        if event.get("event") == "finish":
                            if event.get("code") != "success" or not audio_bytes:
                                raise RuntimeError(f"TTS finish: {event.get('code')}")
                            stamp("tts_prebuffered")
                            record["tts_bytes"] = audio_bytes
                            return
        finally:
            ready.set()
            raw.close()

    sent = 0
    sealed = False

    def send(text, final=False):
        nonlocal sent, sealed
        if sealed or not text or not ready.is_set():
            return
        ws = voice.get("socket")
        if not ws:
            raise ConnectionError("TTS handshake failed")
        end = first_clause(text)
        target = end or len(text)
        piece = text[sent:target]
        if piece and (final or end or len(piece.encode()) >= (42 if sent else 18) or
                      any(c in piece for c in "。！？.!?,;:\n，")):
            ws.send(json.dumps({"event": "text", "text": piece}, ensure_ascii=False))
            stamp("tts_first_text")
            sent = target
        if end or final:
            ws.send('{"event":"finish_text"}')
            stamp("tts_clause_committed")
            record["tts_clause_bytes"] = len(text[:sent].encode())
            sealed = True

    with ThreadPoolExecutor(max_workers=1) as executor:
        audio = None if args.chat_only else executor.submit(get_audio)
        try:
            chat.request("POST", "/chat/completions", body, {**auth, "Content-Type": "application/json"})
            stamp("chat_request_sent")
            response = chat.getresponse()
            stamp("chat_headers")
            if response.status != 200:
                raise RuntimeError(f"Chat HTTP {response.status}")
            answer = ""
            for raw in response:
                line = raw.strip()
                if not line.startswith(b"data:"):
                    continue
                data = line[5:].strip()
                if data == b"[DONE]":
                    stamp("chat_done")
                    break
                choices = json.loads(data).get("choices", [])
                delta = choices[0]["delta"].get("content") if choices else None
                if delta:
                    stamp("chat_first_token")
                    answer += delta
                    if audio is not None:
                        send(answer)
            if not answer or "chat_done" not in record:
                raise RuntimeError("Incomplete chat stream")
            record["answer_bytes"] = len(answer.encode())
            if audio is not None:
                if not ready.wait(12):
                    raise TimeoutError("TTS handshake timeout")
                send(answer, final=True)
                audio.result(timeout=30)
        finally:
            chat.close()
            if not sealed and voice.get("socket"):
                voice["socket"].close()
    record["after_send_to_token_ms"] = record["chat_first_token"] - record["chat_request_sent"]
    if not args.chat_only:
        record["token_to_pcm_ms"] = record["tts_first_pcm"] - record["chat_first_token"]
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key-file", type=Path)
    parser.add_argument("--runs", type=int, default=1, help="trials per connection mode")
    parser.add_argument("--mode", choices=("both", "cold", "warm"), default="both")
    parser.add_argument("--chat-only", action="store_true", help="measure LLM without requesting speech")
    parser.add_argument("--prompt", default=STORY)
    parser.add_argument("--bind-ip")
    parser.add_argument("--dns-server", help="IPv4 DNS on the same LAN as the board")
    parser.add_argument("--interface-index", type=int, help="Windows IPv4 egress interface")
    parser.add_argument("--json", type=Path, help="write timing and route data only")
    args = parser.parse_args()
    if args.runs < 1 or (args.interface_index and sys.platform != "win32"):
        parser.error("Invalid trial count or interface selection")
    key = (args.key_file.read_text(encoding="utf-8-sig") if args.key_file else os.environ.get("DEEPSEEK_API_KEY", "")).strip()
    if not key:
        parser.error("Provide --key-file or DEEPSEEK_API_KEY")
    rows = []
    for i in range(args.runs):
        modes = (False, True) if args.mode == "both" else (args.mode == "warm",)
        for warm in modes[::(-1 if i % 2 else 1)]:
            row = one_trial(args, key, warm)
            rows.append(row)
            speech = "" if args.chat_only else (f" token→PCM {row['token_to_pcm_ms']:4}"
                                                f" start→16KiB {row['tts_prebuffered']:4}")
            print(f"{row['mode']:4} DNS {row['chat_dns_ms']:3} TCP {row['chat_tcp_ms']:3} "
                  f"TLS {row['chat_tls_ms']:3} sent→token {row['after_send_to_token_ms']:4} "
                  f"start→token {row['chat_first_token']:4}{speech} ms", flush=True)
    for mode in ("cold", "warm"):
        times = [r["chat_first_token"] for r in rows if r["mode"] == mode]
        if times:
            print(f"{mode} median time to first token: {statistics.median(times):g} ms (n={len(times)})")
    if args.json:
        args.json.write_text(json.dumps({"utc": datetime.now(timezone.utc).isoformat(),
            "prompt": args.prompt, "times_relative_to": "chat start, milliseconds",
            "trials": rows}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
