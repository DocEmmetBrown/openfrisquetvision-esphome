# Resuming work (state at 01/11/2026 13:40)

Everything needed to pick the project up again. Detailed history and measurements live in [DESIGN.md](DESIGN.md) §10
(windows 10.1 → 10.31); the radio protocol: **[PROTOCOL-REFERENCE.md](PROTOCOL-REFERENCE.md)** (standalone reference) and [PROTOCOL.md](PROTOCOL.md) (discovery log).

## 1. Where things stand

| Item | State |
|---|---|
| Board (Heltec V4.3, `10.35.5.18`) | **ESPHome `0.7.29-phase-2d-associate`** (`dev/phase-2d-associate.yaml`): **it is the boiler's Connect** since 28/10 22:29 |
| Boiler | zone 1 **Hors gel** (8,0 °C), original programme, consignes **19,0** (set by the user on the satellite, 31/10 12:22) / 17,0 / 8,0 ; ECS **Eco Horaires** (state the user wants to keep) |
| Zone block on the board | confirmed; Hors gel block taken by the satellite 01/11 13:37:07, verified (state `20`, mode `10`) |
| Pairing | Learned and saved in flash: NetworkID `05:D9:8D:C9` (same as YAML), **association `D5`** (YAML still says `E3`) |
| MQTT firmware | **No longer paired** (still `E3`): it must be re-associated before it can be the Connect again (see §2) |
| Unattended running | `api: reboot_timeout: 0s`, so no reboot without an API client; emergency stop restored OFF; `explore: true` (exploratory reads only when requested); `satellite_override: true` + `satellite_association: 0xFA` (direct consigne with zone 1 writes) |
| Git | branch `main`, **nothing pushed** |
| Host tests | `tests/host/run.sh` → 924 checks, 0 failures |

Roadmap (DESIGN §10):

- **Done on air**: R, 0, 1, 2a, 2b, 2c (zone 1 setpoints, mode incl. **derogations**, programme incl. edge cases; block
  persistence), **2d** (real Connect association, persistence across reboot), all 6 ECS modes, frame inventory, memory map.
  Zones 2/3: not testable (zone 1 only).
- **Not started**: 3 (satellites, outdoor probe: research done, DESIGN 10.22), 4 (programme ownership, persistent overrides),
  5 (finishing, CI, other boards, first tag).

## 2. Next steps

00. **Direct consigne can stop heating but not start it** (DESIGN 10.28): with the boiler's zone block in Hors gel, a
   Confort consigne in the satellite's name leaves the boiler in standby (`20`). Demand starts at the first satellite frame
   after the satellite has accepted a Confort block. (A `0x40` seen at 13:30:05 was a bit error: CRC invalid, right with `00`.)
   The user wants an immediate start. **Recipe found** (DESIGN 10.29, 0.7.21): block written in the satellite's name
   (`SATZ <mode>`) + consigne in its name with the last word `A032 = 0000` and the mode byte = the block's options byte,
   sent at least ~50 s after the block ⇒ demand within seconds. **Automatic since 0.7.25** on every zone 1 change from HA
   (consigne repeated 35 s and 70 s after the block); verified end to end 15:24 – 15:25 (demand 40 s after the click).
   A new zone request replaces a pending write (0.7.26). **Boost done** (DESIGN 10.30, 0.7.28): switch « Zone 1 boost »,
   comfort +1.0 C with options 0x40, verified on the satellite; stopped after 6 h.
   **0.7.29** (DESIGN 10.31): SX1262 boosted RX gain on (`rx_boosted_gain: true`; measure satellite reception against
   15 valid of ~24 regular exchanges before); satellite-silence guard (nothing in its name after 25 min without it). The satellite also accepts block writes at `:x9:14`, and right
   after a press on « i ».
0. **Dual path: done** (DESIGN 10.26): zone 1 changes from HA take effect on the boiler within a second (direct consigne in
   the satellite's name), the satellite follows within 1-10 minutes. Logs: `build/logs/frisq-dual2.log`.
1. **Live burner state: done** (DESIGN 10.23 – 10.24). « Chaudière brûleur » = last flue snapshot > 40 °C while there is a heating
   demand (state bit `0x08`; snapshots `79E6` at `:x3` and `7A02` at `:x8`), otherwise the slope of the live heating-body
   temperature `A05E` (hot water, 30-45 s lag). Possible follow-ups: ignore the slope ~2 min after a heating demand ends; set
   `explore: false` once mapping is no longer needed.
   Other open points: the RSSI of short frames is meaningless (read it per packet). The satellite's own frames **are received**
   since it was moved on 31/10 (≈ −100 dBm; DESIGN 10.27).
   Tools: `dev/ofv_frames.py` (frame inventory), `dev/ofv_scan.py --split` + `dev/ofv_memmap.py` (memory map),
   `dev/ofv_setpoints.sh` (chained zone writes, each verified), `dev/ofv_api.py get|set|select|text|press`.
2. **Satellite tests with the user: done 01/11** (DESIGN 10.27): satellite frames received; +/− in Auto change the comfort
   setpoint of the block (no derogation); programme edits relayed in ~3 s; ambient only every 10 min unless the satellite is awake.
   Remaining user-chosen tests (autonomous, log `build/logs/frisq-1101-tests.log`):
   - **6**: dual path on a mode change (Hors gel → Confort → Hors gel, heating starts), a derogation, a programme change in Auto.
   - **7**: direct consigne requested inside the quiet window (0.7.18 behaviour).
   - **8**: read `79E0 x1C` and compare with the frisquet-bridge layout (instantaneous DHW at `79EC`, power words 7/8).
   - **9**: burner sensor over heating cycles (`dev/ofv_burner.py`).
   - Optional: satellite absent (batteries out 20–30 min).
   Always finish in Hors gel, ECS Eco Horaires, original programme, consignes 19,0 / 17,0 / 8,0.
3. **Decide which firmware stays the Connect**:
   - ESPHome: change the YAMLs' `association:` to `0xD5` so a « oublier le réseau appris » does not fall back to `E3`
     (which the boiler no longer accepts); remove the old MQTT devices from Home Assistant.
   - MQTT: flash it, then on the boiler « Partenaires > Effacer > Connect », « Ajouter > Connect », and press the portal's
     « Associer Connect » before the last OK (the offer comes ~7 s after it).
4. **If ESPHome stays, tidy up**: one production config instead of the `dev/phase-*.yaml` ones, logger below DEBUG, choose the
   exposed entities.
5. **Phase 3** (needs the user's go-ahead: it changes the heating regulation): outdoor probe fed from HA (device `0x20`, to pair
   on the boiler, writes `A029` every 10 min at `:x4`), or the zone ambient from an HA sensor (writes at `A02F`, as satellite `08`
   / association `FA`; the physical satellite rewrites it every 10 min).
6. **Phase 4**: programme ownership (hybrid, decision D1) and persistent overrides; HA blueprint.
7. **Phase 5**: dev tools, migration guide from the MQTT firmware, CI for the host tests, other boards (V3, LilyGo),
   documentation, first tag.
8. **Open decisions**: pushing the repository; the upstream licence question (D6).
9. **Features requested on 31/10** (not started):
   - **Burner detection while heating**: done 31/10 (DESIGN 10.24): heating demand ⇒ flue snapshot (> 40 °C, twice per
     10 min), otherwise the body-temperature slope (hot water). Possible refinement: ignore the slope for ~2 min after a
     heating demand ends (the body keeps rising when the pump stops).
   - **Send zone writes when the satellite is awake**: an off-schedule boiler→satellite state answer means the satellite is
     awake (info button, or local change): send the pending burst at once, even in the quiet window (DESIGN 10.24).
   - **Merge zone requests**: a request made while a zone write is pending is refused today; merge it into the pending block
     (one write carrying mode + setpoint…), keeping echo matching correct.
   - **OLED screen** of the Heltec V4.3 (0.96" SSD1306 128×64, I2C; check SDA/SCL/RST pins and the Vext power pin): zone mode,
     setpoint and ambient, burner, ECS mode, radio state.
   - **Home Assistant dashboard**: ready-to-paste Lovelace YAML (HA has no dashboard blueprints), plus optional automation
     blueprints (e.g. timed derogation, ECS schedule). Idea from philippemezzadri/frisquet-esphome (Eco Radio System, wired,
     not applicable to Visio): expose an ESPHome **`climate` entity** (mode, target, ambient, « heating » action from the burner)
     for a proper thermostat card, and use **`homeassistant` sensors** as inputs for phase 3 (outdoor temperature, ambient).
   - **Protocol reference**: done, [PROTOCOL-REFERENCE.md](PROTOCOL-REFERENCE.md) (standalone, to reimplement without this code).
   - **From frisquet-bridge** (markkvdb, MIT, same Visio protocol; PROTOCOL-REFERENCE §12):
     - **Boiler pushes**: type `10` seen here (ECS change on the panel, `A0F0 x0D`, ECS mode at `A0FC`); acknowledged since
       0.7.19 (reception `85`), verified on air 01/11 13:03 (no more repeats, HA ECS mode follows at once). `45` never seen.
     - Tip for users: pressing the satellite's **info button** wakes it and makes a pending zone write land at once.
     - `79E0` sensor block has more fields (DHW and heating power, **instantaneous DHW** at `79EC`, zone values).
     - Control modes worth considering for phase 3: **virtual/simple satellite** (own zone 1 via the `A02F` consigne, ambient
       from an HA sensor, physical satellite removed), **central boiler** (TRV demand → one switch), **flow temperature from HA**
       (boiler in outdoor-only regulation, virtual outdoor temperature through the probe identity), outdoor-temperature
       number entity.
     - Their MIT licence may help the upstream-licence question (D6).

Zones 2 and 3 cannot be validated on air (this installation has zone 1 only).

**Pairing procedure learned on 28/10**: with a Connect already paired, « Ajouter » answers « terminé » at once and sends nothing.
Delete it first (« Effacer > Connect »), start the listen (« Radio associer le Connect »), then press the last OK: the offer
arrives ~7 s later.

**Logs of the test windows** (27–28/10) are kept in `build/logs/` (gitignored, no secrets).

## 3. Procedures

All commands from the repository root. `. dev/ofv-env.sh` exports the Wi-Fi credentials (read from
`build/mqtt-config-backup.json`, never printed), `PLATFORMIO_CORE_DIR` and `OFV_DEVICE`.

Files outside git (all in `build/`, gitignored):

- `build/mqtt-b1e6934-heltec_V4_3.bin`: MQTT firmware rollback image (sha256 `986ee130…f3bf`).
- `build/mqtt-config-backup.json`: MQTT portal config export of 25/10 (Wi-Fi, portal password; mode 600).
- `dev/secrets.yaml`: ESPHome API key and OTA password (gitignored).

**Build an ESPHome image**

```sh
. dev/ofv-env.sh && .venv/bin/esphome compile dev/phase-2d-associate.yaml   # the config running on the board
grep -c -a 0.7.7-phase-2d-associate dev/.esphome/build/frisquette/build/firmware.ota.bin   # must be > 0
```

All dev configs share the device name `frisquette`, hence one build directory: always check the version string of the image
before flashing it.

**MQTT → ESPHome** (through the MQTT portal, login password `admin`):

```sh
cp dev/.esphome/build/frisquette/build/firmware.ota.bin /tmp/ofv.ota.bin
J=$(mktemp); curl -s -c $J -o /dev/null --data-urlencode "password=admin" http://10.35.5.18/login
curl -s -m300 -b $J -F "update=@/tmp/ofv.ota.bin;filename=firmware.bin" http://10.35.5.18/api/update; rm -f $J
# expect {"ok":true}, then the ESPHome API on port 6053 within ~20 s
```

**ESPHome → ESPHome** (reboot test, new image): `. dev/ofv-env.sh && .venv/bin/esphome upload dev/<config>.yaml --device 10.35.5.18`

**Logs** (keep this running during the whole window): 
`. dev/ofv-env.sh && .venv/bin/esphome logs dev/<config>.yaml --device 10.35.5.18 > /tmp/frisq.log 2>&1`

**Read / act through the API**: `.venv/bin/python dev/ofv_api.py get "Radio réseau" "bloc mémorisé"`,
`… set "Zone 1 consigne confort" 20.5`, `… press "Radio récupérer le NetworkID"`.

**Rollback ESPHome → MQTT** (since 28/10 the MQTT firmware is **not paired**: re-associate it right after, see §2):

```sh
. dev/ofv-env.sh && .venv/bin/esphome upload dev/phase-2d-pairing.yaml --device 10.35.5.18 --file build/mqtt-b1e6934-heltec_V4_3.bin
# then check the MQTT dashboard: login as above, GET http://10.35.5.18/api/dashboard
# expected: mqttConnected true, boiler.anomaly null, hotWaterMode "Eco Horaires", zone 1 Auto 20.0
```

**Rebuild the MQTT rollback image** (if `build/` was cleaned): the repo is `~/src/OpenFrisquetVisio` (branch
`fix/restore-network-id-after-pairing`, `b1e6934`). Its `heltec_wifi_lora_32_V4` board is not in the installed platforms:

```sh
B=$TMPDIR/ofv_build && rm -rf $B && mkdir -p $B/boards $B/variants/heltec_V4 && cd ~/src/OpenFrisquetVisio
rsync -a --exclude .git --exclude .pio ./ $B/
git show feat/heltec-v4-support:boards/heltec_wifi_lora_32_V4.json > $B/boards/heltec_wifi_lora_32_V4.json
git show feat/heltec-v4-support:variants/heltec_V4/pins_arduino.h > $B/variants/heltec_V4/pins_arduino.h
cat > $B/platformio.tmp.ini <<'E'
[platformio]
extra_configs = platformio.ini
default_envs = tmp

[env:tmp]
extends = env:heltec_wifi_lora_32_V4_3
platform = espressif32@7.0.0
board_build.variants_dir = variants
board_build.variant = heltec_V4
upload_port = 10.35.5.18
extra_scripts =
	pre:tools/generate_web_assets.py
E
cd $B && ~/.platformio/penv/bin/pio run --project-conf platformio.tmp.ini   # → .pio/build/tmp/firmware.bin
```

## 4. Things to remember

- **API reboot**: ESPHome reboots after 15 min without an API client when `api:` has no `reboot_timeout` (all configs except
  `phase-2d-associate.yaml`, which sets `0s`). During a window with another config, keep the log stream connected.
- **Zone block**: the boiler refuses to read `0xA154`. After a boot the block is « restauré », writes are refused until a relay
  or the satellite's broadcast (`08 80 FA`, irregular at ≈ −100 dBm, up to ~10 min) confirms it.
- **Satellite slot**: writes were accepted at `:x4:37`, but on 27/10 one was echoed outside it (10.13): not a hard rule.
- **Listen-only must stay short**: after ~11 h without acknowledgements the boiler showed « anomalie avec Connect » (10.3).
  Every ESPHome config from 2a on acknowledges relays.
- **Tooling**: `tests/host/run.sh` needs `c++` (Xcode command-line tools); it failed inside the Copilot sandbox, which also blocks
  `~/src/OpenFrisquetVisio`. The OTA has no bootloader rollback: keep each image in the same family (board, flash options).
