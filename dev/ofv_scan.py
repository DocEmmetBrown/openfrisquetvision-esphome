"""Memory scan through the exploratory read entity (run from the repository root with .venv/bin/python).

  dev/ofv_scan.py <label> <start> <end> [words]     e.g. dev/ofv_scan.py off 7900 7AFF
  dev/ofv_scan.py <label> --blocks 7A00:1C 79E0:10  only the listed blocks

Reads [start, end] in blocks of `words` (hexadecimal, default 1C) and appends one JSON line per block to
build/logs/scan-<label>.jsonl: {"t": time, "addr": "7A00", "words": 28, "data": "hex" | null}.
Needs `explore: true` and a `type: explore` text entity named "Radio lecture exploratoire".
"""
import asyncio
import json
import re
import sys
import time

import aioesphomeapi

HOST = "10.35.5.18"
ENTITY = "Radio lecture exploratoire"
KEY = re.search(r'^api_key:\s*"?([^"\n]+)"?', open("dev/secrets.yaml").read(), re.M).group(1)
# Reads wait out the satellites' window (:09:30 to :02:30, five minutes).
TIMEOUT_S = 360


def blocks_from_args(args):
    if args[0] == "--blocks":
        out = []
        for item in args[1:]:
            addr, words = item.split(":")
            out.append((int(addr, 16), int(words, 16)))
        return out
    start, end = int(args[0], 16), int(args[1], 16)
    words = int(args[2], 16) if len(args) > 2 else 0x1C
    return [(a, min(words, end - a + 1)) for a in range(start, end + 1, words)]


async def main():
    label = sys.argv[1]
    blocks = blocks_from_args(sys.argv[2:])
    client = aioesphomeapi.APIClient(HOST, 6053, None, noise_psk=KEY)
    await client.connect(login=True)
    entities, _ = await client.list_entities_services()
    entity = next(e for e in entities if e.name == ENTITY)
    lines: asyncio.Queue = asyncio.Queue()

    def on_log(msg):
        text = msg.message.decode(errors="replace") if isinstance(msg.message, bytes) else str(msg.message)
        if "EXPLORE" in text:
            lines.put_nowait(re.sub(r"\x1b\[[0-9;]*m", "", text))

    client.subscribe_logs(on_log, log_level=aioesphomeapi.LogLevel.LOG_LEVEL_INFO)
    with open(f"build/logs/scan-{label}.jsonl", "a") as out:
        for addr, words in blocks:
            tag = f"EXPLORE {addr:04X}+{words:02X}"
            while not lines.empty():
                lines.get_nowait()
            client.text_command(entity.key, f"{addr:04X} {words:02X}")
            data, status = None, "timeout"
            deadline = time.monotonic() + TIMEOUT_S
            while time.monotonic() < deadline:
                try:
                    line = await asyncio.wait_for(lines.get(), deadline - time.monotonic())
                except asyncio.TimeoutError:
                    break
                if tag not in line or line.rstrip().endswith("queued"):
                    if "refused" in line:
                        status = "refused"
                        break
                    continue
                if "no answer" in line:
                    status = "no answer"
                else:
                    data, status = line.split(f"{tag}: ", 1)[1].strip(), "ok"
                break
            record = {"t": time.strftime("%H:%M:%S"), "addr": f"{addr:04X}", "words": words, "status": status,
                      "data": data}
            out.write(json.dumps(record) + "\n")
            out.flush()
            print(record["t"], record["addr"], words, status, (data or "")[:60], flush=True)
    await client.disconnect()


asyncio.run(main())
