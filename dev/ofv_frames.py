"""Inventory of the frames heard in ESPHome logs (run from the repository root).

  python3 dev/ofv_frames.py build/logs/*.log [--since HH:MM] [--examples]

Groups every "RX <n> bytes" frame by (source, destination, association, reception, type, length, address/body tag)
and prints counts, first/last time, RSSI range and, with --examples, one frame per class.
"""
import re
import sys
from collections import defaultdict

RX = re.compile(r"^\[(\d\d:\d\d:\d\d)\.\d+\].*RX (\d+) bytes [0-9A-F]{2}->[0-9A-F]{2} type=[0-9A-F]{2} rssi=(-?[\d.]+) ([0-9A-F.]+)")
TX = re.compile(r"^\[(\d\d:\d\d:\d\d)\.\d+\].*TX (\w+) ([\w-]+) ([0-9A-F.]+)")
NAMES = {0x80: "chaudière", 0x7E: "Connect", 0x08: "satellite Z1", 0x09: "satellite Z2", 0x0A: "satellite Z3",
         0x20: "sonde ext.", 0x00: "diffusion"}


def name(b):
    return NAMES.get(b, f"{b:02X}")


def tag(f):
    """A short description of what the frame carries."""
    typ, rec = f[5], f[4]
    if typ == 0x03 and rec < 0x80 and len(f) >= 10:
        return f"lecture {f[6]:02X}{f[7]:02X} x{f[8]:02X}{f[9]:02X}"
    if typ == 0x03:
        return f"réponse lecture, {f[6]} octets"
    if typ == 0x17 and len(f) >= 15 and rec < 0x80 and f[6] != 0x2A:
        return f"échange écrit {f[6]:02X}{f[7]:02X} x{f[8]:02X}{f[9]:02X} / lu {f[10]:02X}{f[11]:02X} x{f[12]:02X}{f[13]:02X}"
    if typ == 0x17 and len(f) > 6:
        return f"réponse échange, {f[6]} octets" if rec >= 0x80 else f"échange, {f[6]} octets"
    if typ == 0x41:
        return "association"
    return f"type {typ:02X}"


def main():
    files = [a for a in sys.argv[1:] if not a.startswith("--")]
    since = next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--since"), None)
    examples = "--examples" in sys.argv
    classes = defaultdict(lambda: {"n": 0, "first": None, "last": None, "rssi": [], "example": None})
    seen = set()
    for path in files:
        for line in open(path, errors="replace"):
            m = RX.match(line)
            if not m:
                continue
            t, _, rssi, hexes = m.groups()
            if since and t < since:
                continue
            key_line = (path, line[:16])
            if key_line in seen:
                continue
            seen.add(key_line)
            f = bytes(int(x, 16) for x in hexes.split("."))
            if len(f) < 6:
                continue
            key = (name(f[1]), name(f[0]), f"{f[2]:02X}", f"{f[4]:02X}", f"{f[5]:02X}", len(f), tag(f))
            c = classes[key]
            c["n"] += 1
            c["first"] = c["first"] or t
            c["last"] = t
            c["rssi"].append(float(rssi))
            c["example"] = c["example"] or hexes
    for key, c in sorted(classes.items(), key=lambda kv: -kv[1]["n"]):
        src, dst, assoc, rec, typ, length, what = key
        r = c["rssi"]
        print(f"{c['n']:5d}  {src:>12} -> {dst:<12} assoc {assoc} réc {rec} type {typ} {length:2d} o  {what:<45} "
              f"{c['first']}..{c['last']}  rssi {min(r):.0f}..{max(r):.0f}")
        if examples:
            print(f"       {c['example']}")


main()
