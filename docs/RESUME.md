# Resuming work (state at 27/10/2026 12:20)

Everything needed to pick the project up again. Detailed history and measurements live in [DESIGN.md](DESIGN.md) §10
(windows 10.1 → 10.15); the radio protocol is in [PROTOCOL.md](PROTOCOL.md).

## 1. Where things stand

| Item | State |
|---|---|
| Board (Heltec V4.3, `10.35.5.18`) | **MQTT firmware** `b1e6934` (portal version `26.10.24.1813`), MQTT connected, no boiler anomaly, ECS « Eco Horaires », zone 1 Auto 20,0 °C |
| ESPHome preferences on the board | Kept across MQTT periods: zone 1 block saved at 11:47 (comfort 20,0 / reduced 17,0 / frost 8,0). **No learned network** (YAML `05:D9:8D:C9`, association `E3`) |
| Git | branch `main`, everything committed, **nothing pushed**. Last commits: `a2f5ffc`, `6a5bfaf` (phase 2d), `ede498e`, `a599b71`, `ce6cae4` |
| Host tests | `tests/host/run.sh` → 669 checks, 0 failures |

Roadmap (DESIGN §10):

- **Done on air**: R, 0, 1, 2a, 2b, **2c** (zone 1 setpoints, mode, programme; block persistence; write from a restored block).
- **2d (pairing)**: code and host tests done; on air only the *timeout* path was checked (10.15). Zones 2/3: not testable (zone 1 only).
- **Not started**: 3 (satellites, outdoor probe), 4 (programme and overrides), 5 (finishing, CI, other boards, first tag).

## 2. Next steps

1. **NetworkID recovery with a real offer** (listen only, safe): flash `dev/phase-2d-pairing.yaml`, the user puts the boiler in
   pairing mode, press « Radio récupérer le NetworkID » within the same 30 s. Expect `Association offer received: …` in the logs,
   `Pairing: succeeded`, sensor « Radio réseau » = `05:D9:8D:C9 (appris)` (same ID). Record the offer bytes in PROTOCOL.md and add
   them to `tests/fixtures/frames.txt` + a test. Afterwards press « oublier » if the YAML should stay the reference.
2. **Connect association** (only if the user wants it): needs `pairing: true` in the YAML. **It changes the Connect's association
   byte**: the MQTT firmware (still `E3`) must then be re-associated before rolling back to it. Decide first.
3. Then phase 3 (satellites / outdoor probe), or a long-running ESPHome trial (see the 15-minute API reboot below).

## 3. Procedures

All commands from the repository root. `. dev/ofv-env.sh` exports the Wi-Fi credentials (read from
`build/mqtt-config-backup.json`, never printed), `PLATFORMIO_CORE_DIR` and `OFV_DEVICE`.

Files outside git (all in `build/`, gitignored):

- `build/mqtt-b1e6934-heltec_V4_3.bin`: MQTT firmware rollback image (sha256 `986ee130…f3bf`).
- `build/mqtt-config-backup.json`: MQTT portal config export of 25/10 (Wi-Fi, portal password; mode 600).
- `dev/secrets.yaml`: ESPHome API key and OTA password (gitignored).

**Build an ESPHome image**

```sh
. dev/ofv-env.sh && .venv/bin/esphome compile dev/phase-2d-pairing.yaml
grep -c -a 0.7.0-phase-2d-pairing dev/.esphome/build/frisquette/build/firmware.ota.bin   # must be > 0
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

**Rollback ESPHome → MQTT** (always at the end of a window):

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

- **API reboot**: ESPHome reboots after 15 min without an API client (`api:` has no `reboot_timeout`). Keep the log stream
  connected during a window; for a long trial set `reboot_timeout: 0s` or connect Home Assistant.
- **Zone block**: the boiler refuses to read `0xA154`. After a boot the block is « restauré », writes are refused until a relay
  or the satellite's broadcast (`08 80 FA`, irregular at ≈ −100 dBm, up to ~10 min) confirms it.
- **Satellite slot**: writes were accepted at `:x4:37`, but on 27/10 one was echoed outside it (10.13): not a hard rule.
- **Listen-only must stay short**: after ~11 h without acknowledgements the boiler showed « anomalie avec Connect » (10.3).
  Every ESPHome config from 2a on acknowledges relays.
- **Tooling**: `tests/host/run.sh` needs `c++` (Xcode command-line tools); it failed inside the Copilot sandbox, which also blocks
  `~/src/OpenFrisquetVisio`. The OTA has no bootloader rollback: keep each image in the same family (board, flash options).
