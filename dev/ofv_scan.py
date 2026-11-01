"""Memory scan through the exploratory read entity (run from the repository root with .venv/bin/python).

  dev/ofv_scan.py <label> <start> <end> [words] [--split]   e.g. dev/ofv_scan.py off 7900 7AFF --split
  dev/ofv_scan.py <label> --blocks 7A00:1C 79E0:10          only the listed blocks

Reads [start, end] in blocks of `words` (hexadecimal, default 1C) and appends one JSON line per read to
build/logs/scan-<label>.jsonl: {"t", "addr", "words", "status": ok | refused | no answer | timeout, "data"}.
With --split, a refused block is read again as two halves, down to single words: the boiler refuses a whole read
as soon as one word of it is invalid.
Needs `explore: true` and a `type: explore` text entity named "Radio lecture exploratoire".
"""
import asyncio
import os
import json
import re
import sys
import time

import aioesphomeapi

HOST = os.environ.get("OFV_DEVICE", "10.35.5.140")
ENTITY = "Radio lecture exploratoire"
KEY = re.search(r'^api_key:\s*"?([^"\n]+)"?', open("dev/secrets.yaml").read(), re.M).group(1)
# Reads wait out the satellites' window (:09:30 to :02:30, five minutes).
TIMEOUT_S = 360


def blocks_from_args(args):
    args = [a for a in args if a != "--split"]
    if args[0] == "--blocks":
        out = []
        for item in args[1:]:
            addr, words = item.split(":")
            out.append((int(addr, 16), int(words, 16)))
        return out
    start, end = int(args[0], 16), int(args[1], 16)
    words = int(args[2], 16) if len(args) > 2 else 0x1C
    return [(a, min(words, end - a + 1)) for a in range(start, end + 1, words)]


class Reader:
    def __init__(self, client, entity):
        self.client = client
        self.entity = entity
        self.lines: asyncio.Queue = asyncio.Queue()

    def on_log(self, msg):
        text = msg.message.decode(errors="replace") if isinstance(msg.message, bytes) else str(msg.message)
        if "EXPLORE" in text:
            self.lines.put_nowait(re.sub(r"\x1b\[[0-9;]*m", "", text))

    async def read(self, addr, words):
        tag = f"EXPLORE {addr:04X}+{words:02X}"
        while not self.lines.empty():
            self.lines.get_nowait()
        self.client.text_command(self.entity.key, f"{addr:04X} {words:02X}")
        deadline = time.monotonic() + TIMEOUT_S
        while time.monotonic() < deadline:
            try:
                line = await asyncio.wait_for(self.lines.get(), deadline - time.monotonic())
            except asyncio.TimeoutError:
                break
            if tag not in line or line.rstrip().endswith("queued"):
                continue
            if "boiler refused" in line:
                return "refused", None
            if "no answer" in line:
                return "no answer", None
            if "refused (" in line:
                return "not sent", None
            return "ok", line.split(f"{tag}: ", 1)[1].strip()
        return "timeout", None


async def main():
    label = sys.argv[1]
    split = "--split" in sys.argv
    pending = blocks_from_args(sys.argv[2:])
    client = aioesphomeapi.APIClient(HOST, 6053, None, noise_psk=KEY)
    await client.connect(login=True)
    entities, _ = await client.list_entities_services()
    reader = Reader(client, next(e for e in entities if e.name == ENTITY))
    client.subscribe_logs(reader.on_log, log_level=aioesphomeapi.LogLevel.LOG_LEVEL_INFO)
    with open(f"build/logs/scan-{label}.jsonl", "a") as out:
        while pending:
            addr, words = pending.pop(0)
            status, data = await reader.read(addr, words)
            record = {"t": time.strftime("%H:%M:%S"), "addr": f"{addr:04X}", "words": words, "status": status,
                      "data": data}
            out.write(json.dumps(record) + "\n")
            out.flush()
            print(record["t"], record["addr"], words, status, (data or "")[:60], flush=True)
            if split and status == "refused" and words > 1:
                half = words // 2
                pending[0:0] = [(addr, half), (addr + half, words - half)]
    await client.disconnect()


asyncio.run(main())
