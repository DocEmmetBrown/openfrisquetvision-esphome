"""Readable memory ranges from exploratory scans (run from the repository root).

  python3 dev/ofv_memmap.py build/logs/scan-map.jsonl [more.jsonl ...]

Merges every "ok" read into a word map and prints the contiguous readable ranges with their values
(the latest read of a word wins), 8 words per line.
"""
import json
import sys


def main():
    words = {}
    for path in sys.argv[1:]:
        for line in open(path):
            r = json.loads(line)
            if r["status"] != "ok" or not r["data"]:
                continue
            base = int(r["addr"], 16)
            data = r["data"]
            for i in range(len(data) // 4):
                words[base + i] = data[4 * i:4 * i + 4]
    addrs = sorted(words)
    ranges = []
    for a in addrs:
        if ranges and a == ranges[-1][1] + 1:
            ranges[-1][1] = a
        else:
            ranges.append([a, a])
    for start, end in ranges:
        print(f"{start:04X}-{end:04X} ({end - start + 1} mots)")
        for a in range(start, end + 1, 8):
            row = " ".join(words[w] for w in range(a, min(a + 8, end + 1)))
            print(f"  {a:04X}: {row}")


main()
