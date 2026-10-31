# Resuming work (state at 31/10/2026 11:25)

Everything needed to pick the project up again. Detailed history and measurements live in [DESIGN.md](DESIGN.md) §10
(windows 10.1 → 10.23); the radio protocol: **[PROTOCOL-REFERENCE.md](PROTOCOL-REFERENCE.md)** (standalone reference) and [PROTOCOL.md](PROTOCOL.md) (discovery log).

## 1. Where things stand

| Item | State |
|---|---|
| Board (Heltec V4.3, `10.35.5.18`) | **ESPHome `0.7.12-phase-2d-associate`** (`dev/phase-2d-associate.yaml`): **it is the boiler's Connect** since 28/10 22:29 |
| Boiler | zone 1 **Hors gel** (8,0 °C), original programme, consignes 20,0 / 17,0 / 8,0 ; ECS **Eco Horaires** (state the user wants to keep) |
| Pairing | Learned and saved in flash: NetworkID `05:D9:8D:C9` (same as YAML), **association `D5`** (YAML still says `E3`) |
| MQTT firmware | **No longer paired** (still `E3`): it must be re-associated before it can be the Connect again (see §2) |
| Unattended running | `api: reboot_timeout: 0s`, so no reboot without an API client; emergency stop restored OFF; `explore: true` (exploratory reads only when requested) |
| Git | branch `main`, **nothing pushed** |
| Host tests | `tests/host/run.sh` → 778 checks, 0 failures |

Roadmap (DESIGN §10):

- **Done on air**: R, 0, 1, 2a, 2b, 2c (zone 1 setpoints, mode incl. **derogations**, programme incl. edge cases; block
  persistence), **2d** (real Connect association, persistence across reboot), all 6 ECS modes, frame inventory, memory map.
  Zones 2/3: not testable (zone 1 only).
- **Not started**: 3 (satellites, outdoor probe: research done, DESIGN 10.22), 4 (programme ownership, persistent overrides),
  5 (finishing, CI, other boards, first tag).

## 2. Next steps

1. **Live burner state: done** (DESIGN 10.23). « Chaudière brûleur » follows the slope of the live heating-body temperature
   `A05E` (read every 15 s), with ~30-45 s of lag; « Chaudière corps de chauffe (direct) » is the raw value. Possible follow-ups:
   tune the thresholds over a few days of heating cycles; set `explore: false` once mapping is no longer needed.
   Other open points: the satellite's own frames are never received, even at 467 kHz RX bandwidth (test below); the RSSI of
   short frames is meaningless (read it per packet).
   Tools: `dev/ofv_frames.py` (frame inventory), `dev/ofv_scan.py --split` + `dev/ofv_memmap.py` (memory map),
   `dev/ofv_setpoints.sh` (chained zone writes, each verified), `dev/ofv_api.py get|set|select|text|press`.
2. **Satellite tests with the user (planned, not done yet)**: the satellite runs on batteries and can be moved, no special functions.
   Start a capture (`esphome logs … > build/logs/frisq-sat.log`), then:
   - **Range**: bring the satellite next to the board and change a setting on it (it triggers an exchange at once). Look for
     `08 -> 80` frames with `python3 dev/ofv_frames.py build/logs/frisq-sat.log`. Received → it was range; decode what it writes
     at `A02F` (ambient, setpoint, mode). Still nothing → different bit rate / modulation / sync word.
   - **Local changes**: +/− setpoint in Auto, derogation if the satellite offers one (and its end at the next transition),
     programme edit on the satellite. Each: relay `08`, broadcast byte, HA entities; then back to Hors gel / original programme.
   - **Ambient**: warm the satellite in a hand; read `A029` every minute (already scheduled from :03 to :09) to see when the
     ambient is updated (only at :x0:08, or on change).
   - **Satellite absent** (optional): batteries out 20–30 min; boiler reaction, `A029`, anomaly bit; check recovery.
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
   - **Burner detection while heating**: test running on 31/10 (zone 1 Confort, comfort 21,5 °C) — the `A05E` slope may miss
     a modulating burner with the pump running. Restore Hors gel and comfort 20,0 °C afterwards.
   - **Merge zone requests**: a request made while a zone write is pending is refused today; merge it into the pending block
     (one write carrying mode + setpoint…), keeping echo matching correct.
   - **OLED screen** of the Heltec V4.3 (0.96" SSD1306 128×64, I2C; check SDA/SCL/RST pins and the Vext power pin): zone mode,
     setpoint and ambient, burner, ECS mode, radio state.
   - **Home Assistant dashboard**: ready-to-paste Lovelace YAML (HA has no dashboard blueprints), plus optional automation
     blueprints (e.g. timed derogation, ECS schedule).
   - **Protocol reference**: done, [PROTOCOL-REFERENCE.md](PROTOCOL-REFERENCE.md) (standalone, to reimplement without this code).

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
