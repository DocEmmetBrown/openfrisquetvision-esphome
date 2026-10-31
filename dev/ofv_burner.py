"""Burner timeline from an ESPHome log (run from the repository root).

  python3 dev/ofv_burner.py build/logs/frisq-heating.log [--since HH:MM]

One line per live heating-body reading (0xA05E): time, temperature, rise since the previous reading, burner sensor,
then the latest boiler state byte (0xA029 / broadcast), zone 1 flow temperature and flue snapshot seen so far.
"""
import re
import sys

LINE = re.compile(r"^\[(\d\d:\d\d:\d\d)\.\d+\]")


def main():
    path = sys.argv[1]
    since = next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--since"), "00:00")
    state = {"burner": "?", "status": "?", "flow": "?", "flue": "?"}
    prev = None
    for line in open(path, errors="replace"):
        m = LINE.match(line)
        if not m or m.group(1) < since:
            continue
        t = m.group(1)
        if "'Chaudière brûleur' >>" in line:
            state["burner"] = line.rsplit(">>", 1)[1].strip()
        elif "Status A029: state=" in line:
            state["status"] = re.search(r"state=([0-9A-F]{2})", line).group(1)
        elif "état=" in line and "octets bruts" in line:
            state["status"] = re.search(r"état=([0-9A-F]{2})", line).group(1)
        elif "'Zone 1 départ' >>" in line:
            state["flow"] = line.rsplit(">>", 1)[1].strip().replace(" °C", "")
        elif "'Chaudière fumées' >>" in line:
            state["flue"] = line.rsplit(">>", 1)[1].strip().replace(" °C", "")
        elif "'Chaudière corps de chauffe (direct)' >>" in line:
            v = float(line.rsplit(">>", 1)[1].split()[0])
            rise = "" if prev is None else f"{v - prev:+.1f}"
            prev = v
            print(f"{t}  corps {v:5.1f} {rise:>5}  brûleur {state['burner']:<3}  état {state['status']}  "
                  f"départ {state['flow']:>5}  fumées(:x8) {state['flue']}")


main()
