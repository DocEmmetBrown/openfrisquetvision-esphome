# External research on the Frisquet radio and Connect (01/11/2026)

This file gathers what other people have published about the Frisquet Visio radio and the Frisquet Connect. It covers forums,
other open-source implementations, the cloud API and official Frisquet documents, and was compared against our own knowledge
([PROTOCOL-REFERENCE.md](PROTOCOL-REFERENCE.md)) and captures (`build/logs`). It is written for the software design work: each
fact gives its **confidence**, its **source**, and what it **implies for the firmware**.

Confidence tags:

- **[own data]**: re-checked against our own scans or captures.
- **[high]**: several independent sources, or a clear capture.
- **[medium]**: one good source.
- **[low]**: a single report, or the researcher's own inference.

Nothing here was tested on our boiler unless it is tagged [own data]. Section 6 lists the experiments.

---

## 1. Sources

Two sources were already known: OpenFrisquetVisio (FreedomNX) and frisquet-bridge (markkvdb).

| Short name | Source | Content |
|---|---|---|
| HACF | https://forum.hacf.fr/t/19814 (≈470 posts, 2023-25), /67032 (≈1,000), /77377 (≈110), /78391 | Community reverse engineering: mgre, Freedom (author of OpenFrisquetVisio), Sacha, Gib, Spanky, haribo77, jnilo, yvesbar. Cited below as `19814/<post>`. |
| Sheet | https://docs.google.com/spreadsheets/d/1nxlOCWJcXsvzIuNGcTc-qPI06q_-CZ6FZuc4tkzhcPI | Shared memory map, tabs: Trames, 7531, 7919, 79E0, 9C40, A029, A0F0, "Trame Erreur" |
| d33d33 | https://github.com/d33d33/frisquet-connect (Rust, RFM69 / SX1262) | Full Connect emulator: pairing, holiday block, zone-block bit layout (`src/connect/*.rs`) |
| PR30, #7 | OpenFrisquetVisio PR #30 (mode bits, zone byte 5), issue #7 (boost capture), PR #25 (consumption), #22 (power unit) | Captures that are not in its README |
| mgre | github.com/mgrenonville/sx1211-arduino, /frisquet-commander, /rtl_433 | Original SX1211 settings, frame parsers with real frames |
| TLeconte | github.com/TLeconte/rtl_433, orphan commit `f9504624d6…`, `src/devices/frisquet.c` | rtl_433 decoder with the correct CRC |
| Others | Burnallover/frisquet-mqtt (+ wiki), hpoussin/ESPsquet, vletoux/OpenFrisquetVisioPiZero, erwannbst branch `fix/satellite-physique-hydroconfort` | Outdoor-sensor emulation, type-10 acknowledgement, Hydroconfort quirk |
| Cloud | mgcrea/homebridge-frisquet-connect (`docs/protocol.md`, fixtures), TheGui01/Frisquet-connect-for-home-assistant, home-assistant-custom-components/frisquet-connect, markkvdb/frisquet-api, Jeedom "Doc_API.txt" | Cloud field names, which mirror boiler variables |
| SAV | Frisquet "Cahier SAV maintenance" (text in `guyaime69800/chaudiere-web3`), installer notices 310685_21 and 310704_07, TEAMAO 310831_03, Connect notice 310801_10 (frisquet.com/fichiers/frisquetconnect-notice.pdf) | Official anomaly texts, menus, compatibility, rules for boost and holidays |

---

## 2. Confirmed in our own data

| Fact | Evidence | Implication |
|---|---|---|
| **`7A16–7A17` is the boiler clock**, not a counter: a 32-bit Unix time stored **low word first**, holding local time as if it were UTC. | `scan-flame2.jsonl` 10:52:28: `c871 6ae5` → 0x6AE5C871 = 2026-10-31 10:52:33 | One read gives the full date and time. It could replace or cross-check `A02B` in `proto_clock.h`. Fix the wording "compteur (+1/s)" in PROTOCOL-REFERENCE §8. |
| **`A0F8–A0F9` = `AE80 66D3`** uses the same encoding: 0x66D3AE80 = **2024-09-01 00:00**. | `scan-reads.jsonl`, read of A0F0 | The "identifier" is a date, probably a setting such as the next-service date. Ask the user whether the service reminder is set to 09/2024. |
| `A0F0–A0F7` = two 8-byte holiday date fields: low-word-first timestamp followed by four zero bytes. End starts at `A0F4`, not `A0F2`. | d33d33 `src/connect/holiday.rs`; live read/write/readback on 2026-11-11 | Write eight words, with no extra byte after the data length. |
| `7931 = 1901` = **panel firmware V19.01**. Freedom's boiler reads 1901 too; `7541` holds the same value on other boilers. | `scan-map-780E.txt` | Can be shown as a diagnostic sensor. |
| `9C54` page: BCD date, status, weekday (**0 = Sunday**), **NetworkID at `9C58–9C59`**, **`0080` = boiler address at `9C5A`** | `scan-map-9C00.txt` (29/10/2026 = 4) | A second source for the NetworkID and the boiler address. |
| `7A07` (byte 42 of our `79F2` read) = `0009` at rest. Sheet: `0005`/`0009` standby, `0007` hot-water heating. | `scan-map-780E.txt` | Agrees with our "operating state". |
| **`A033` = current zone 1 flow target** (`00C6` = 19.8 °C, the minimum); `A032` = previous target [Sacha, 67032/618] | read of `A02A` | Fills the "2 mots [hypothèse]" in PROTOCOL-REFERENCE §7.2. Could be a "flow target" sensor. |
| `7944 = 0006` (zone 1 regulation auto), **`7946 = 143C` (zone 1 flow min / max = 20 / 60 °C)**, `7949`/`794C` = `1450` (20 / 80 °C) | `7942` block | Partly decodes the parameters at `793E–7956` (section 3.4). |

---

## 3. New protocol facts

### 3.1 Physical layer

- **Original chip: Semtech SX1211**, in the boiler panel and in the satellite (the RFM63 module is a clone). The satellite's MCU is
  an NXP MC9S08LL16 (PCB v19) or an STM32L (v27). [high, 19814/3, /9]
- SX1211 settings sniffed from the panel: 50 kHz deviation, 25 kbit/s, PLL registers `R1=7F P1=65 S1=4A` (≈ 868.95 MHz), preamble
  4 bytes, 32-bit sync word with 0 errors tolerated, CRC on, no whitening, 64-byte FIFO.
  [high, mgre `sx1211-arduino/.../main.cpp:185-235`, 19814/208]
- **On air the preamble is 5 bytes of `0xAA`** [medium, Sacha with URH, 77377/2]. Our 16 bits are enough to receive. For
  **sending**, a longer preamble (4–5 bytes, as the SX1211 is set up) may help battery satellites, which listen only briefly.
- **The SX1211 crashes on a length byte above `0x3F`**, so 63 data bytes is a hard limit for anything we send. [medium, 19814/186]
- The bit slip in long runs of `00` is seen by others too (OpenFrisquetVisio logs `[RX][CRC_REPAIRED]`). Our repair method
  stands. [high]
- The NetworkIDs seen so far all start with zero bits (`05…`, `06…`, `00…`). [low]
- **Reception sensitivity**: the SX1262 hears much less than the SX1211 ("one frame in two") [19814/250]. Heltec V4.x boards
  need their front-end chip switched correctly, which we already do. **Our code never enables the SX1262 boosted RX gain**
  (`setRxBoostedGainMode(true)`, register `0x08AC = 0x96`, about +3 dB). This is the cheapest lead for the "satellite frames never
  received" problem.
- rtl_433 settings for debugging with an SDR: `-X 'n=frisquet,m=FSK_PCM,short=38,long=38,reset=4200'`, or `-Y classic` (the
  default demodulator drops strong, nearby packets) [rtl_433 issue #2445].

### 3.2 Header

- The sheet describes header byte 4 (our "réception") as **1 bit request/reply + 7 bits node**:

  | Value | Meaning |
  |---|---|
  | `01` | direct message |
  | `02` | boiler → device directly (pairing, radio test) |
  | `03` | **boiler acting as the requester** (towards the Module H) |
  | `05` | **boiler pushing to the Connect** (types `10`, `45`); the Connect answers `85` |
  | `08`, `7E` | relay |

  [high, Sheet, 67032/714-715]
- The message id rises by **+4 per 10-minute cycle and +1 per retry**. The satellite retries 3 times about 4 s apart, then waits
  10 min. [medium, 19814/441]

### 3.3 Message types and errors

| Type | Meaning | Confidence |
|---|---|---|
| `03` | read | known |
| **`10`** | **write multiple** (Modbus-like). The reply repeats the address and the word count. The boiler pushes it to the Connect with reception `05`, e.g. `7E 80 <assoc> <id> 05 10 A0 F0 00 0D 1A <26 bytes>`. The Connect acknowledges with `80 7E <assoc> <id> 85 10 A0 F0 00 0D` (addresses swapped, same id, no data). | high [67032/714, ESPsquet `src/Connect.cpp:49-55`] |
| `17` | read + write | known |
| `41` | pairing | known (see 3.5 for the payload) |
| **`42`** | **radio test** (signal strength), boiler → device with reception `02`: `…02 42 38 05 8A 8A…`, echoed back with `82` | medium [67032/720] |
| **`43`** | **text alert, not an outdoor-sensor init** (contrary to frisquet-bridge). Low battery: `80 08 00 B4 01 43 00 1C 53 41 54 45 4C 4C 49 54 45 20 5A 31 00 43 48 41 4E 47 45 52 20 50 49 4C 45 53 20 00` ("SATELLITE Z1\0CHANGER PILES \0"), answered `08 80 00 B4 81 43`. "No alert": `43 00 00`. It is also sent when a satellite starts up, and daily at 04:00 by the outdoor sensor. | high [67032/775, 19814/313, /371] |
| `44` | Connect → boiler with data `01 00` or `02 00`; empty acknowledgement. Meaning unknown. | known, meaning unknown |
| **`45`** | **event** pushed by the boiler to the Connect (reception `05`), answered `85`. Data `02 0A <device> <code> <Unix time, little-endian>…`. Examples: device `20` code `00` at 04:00:05 (the daily resync); device `7E` code `01`; device `80` **code `31`**. Best lead for boiler fault codes. | layout: low (researcher's decode, fits 5 frames) |

**Errors.** The reply type is the request type + `0x80`, followed by a **Modbus exception code**. The boiler accepts only `03`, `10`, `17`
and `43`. [high, Sacha's full probe, 67032/637]

| Code | Meaning | Example |
|---|---|---|
| `01` | function not supported | `0880FAFF818401` |
| `02` | address not mapped | our `81 83 02` refusals |
| `03` | bad argument | `0880FAFF818303` |

**Implication**: the decoder (`proto_decode.h`) should name these codes. The engine should **acknowledge `10` and `45` pushes**,
which we do not yet do (section 4.2).

### 3.4 Memory

**Readable areas** on boilers that support the Connect: `7531–7544`, `7919–7961`, `79E1–7A4F`, `9C40–9C5A`, `A029–A093`,
`A0F0–A104`. **We never scanned `7531–7544`.** On boilers without Connect support, only `A029–A05F` and `7531–7544` answer.
[high, 67032/748, /758]

| Address | Content | Confidence |
|---|---|---|
| `7541` | panel firmware version (`1502`, `1504`, `1901`); `7544` changes fast | medium |
| `791C–791F` | association table, one word per device: (association id, device address), e.g. `0A08` satellite, `1B20` outdoor sensor, `AA7E` Connect | high (ours: `FA00 FA08 D57E`) |
| `7944`, `7947`, `794A` | regulation of zone 1, 2, 3: `0006` auto, `0002` manual (`000A` = limited power?) | medium |
| `7946`, `7949`, `794C` | flow **minimum / maximum** for zones 1, 2, 3 (one byte each, in °C) | medium + [own data] |
| `7945`, `7948`, `794B` | ours `4B08`, `8B0F`, `4B0F`; others `4929`. Unknown. Hypothesis: the low byte is the heating-curve slope × 10 (0.8 / 1.5). | low |
| `7957` | low byte = manual flow setpoint in °C | medium |
| `792D – 793D` | "everything drops to 0000 in summer or during a long standby" (sheet note) | low |
| `79E0` block | `79E0` hot water, `79E1` heat exchanger, `79E2–79E4` flow Z1–Z3, `79E5` safety probe, `79E6` flue gas ÷20, `79E7`/`79E8` power hot water / heating, `79EA` pressure, `79EB` state, `79F2–79F4` room Z1–Z3, `79F5–79F7` internal flow setpoint, `79F8–79FA` room setpoint, `79FB` outdoor | high |
| `79E7`/`79E8` | power **unit disputed**: whole kW for most users, 0.1 kW for FreedomNX [#22] | — |
| `7A21`, `7A22` | (words 9 and 10 of `7A18`) **yesterday's** hot-water / heating kWh, frozen at midnight | high [PR25] |
| `7A30–7A33` | **versions of the paired devices**, e.g. `0108 1405 0220 2101` = satellite 08 v14.05, Connect v21.01. **All zeros on our boiler** (section 3.5). | medium |
| `7A34` | read by the real Connect, alternating with `7A18`. Sample values `FF7E`, `017E 2100`. | unknown |
| `9C40` | written to `0000` by the outdoor sensor (04:00) and by the Connect (paired with some reads). Probably an event or alert flag that devices clear. | low |
| `A02E` (high byte) | status bitmap per Freedom: `0x08` running, `0x04` stopped by the dry contact, `0x10`/`0x20` Connect, **`0x80` anomaly** (see section 5) | medium |
| `A032/A033`, `A037/A038`, `A03C/A03D` | previous and current flow target for zones 1, 2, 3 | medium + [own data] |
| `A034` | write target of the zone 2 satellite; the high byte of its mode word holds `0x80` | low [19814/441] |
| `A043–A048` | second copy of the outdoor temperature, date and status | high |
| `A0F0` block | holiday start and end (see 2), date at `A0F8`, DHW mode `A0FC`; the end of the block holds the **14-digit boiler serial in ASCII** (`"23045099530011"` in a d33d33 frame) | medium |

The A154 zone block **lives in the satellite**, not in the boiler (Freedom, 67032/713). This fits our `83 02` refusals.

### 3.5 Pairing

- On real devices, the answer to the pairing offer (`82 41`) **carries a version, not the NetworkID**:

  | Device | Answer payload | Source |
  |---|---|---|
  | Connect | `01 21 01 02` (v21.01.02) | d33d33 `src/connect/pair.rs:86-97` |
  | satellite | `03 23 12 06 01 27 00 02` | 19814/178 |
  | outdoor sensor | `01 21 01 02` | Burnallover `main.cpp:374-376` |

  We echo the NetworkID, like OpenFrisquetVisio. It works, but our `7A30–7A33` read zero. **Hypothesis**: the boiler records the
  version and uses it to enable Connect-only behaviour, such as the `10` and `45` pushes we never receive. [low; experiment 6.4]
- Without the expected answer, the boiler shows "Appareil incorrect". Pair all devices in one session. To re-pair after a link
  loss, use **"Mise en service › Radio › Remplacer › Frisquet Connect"** (Frisquet says to replace, never to erase, when swapping
  a Connect) [SAV p.27-28].
- Minimum versions for a Connect: satellite **V14.05.01**, panel **V19.02.00** [SAV p.26]. Our panel reports 19.01 and works anyway.

### 3.6 What the real Connect does (captures)

- Every 10 min it reads `A02B ×4` (clock), `79E0 ×1C` and `79FC ×1C`, and alternately `7A18 ×1C` and `7A34 ×1C` [issue #7,
  78391/66].
- Around 05:00 it reads `7541` (panel version) and `A0F0` [PR25].
- It never reads `A063`. The cloud adds up the daily `7A18` values itself [PR25].
- It writes the **DHW mode with a type `10` write of `A0F0` × 13 words** (reception `05`), not a one-word `17`. The sequence
  Max / Eco / Stop gave `…80` / `…88` / `…90` [78391/55]. Our one-word `17` write is verified and works; this only matters for
  copying the real Connect exactly.
- It answers `10` and `45` pushes with `85`. After a `45` it reads the satellite block and writes `9C40 = 0000` through the relay.

### 3.7 Zone block A154 and mode bits

- **Byte 5 = `0x80` for a permanent mode**, Confort or Réduit selected without the schedule [PR30, medium]:

  | Block start | Meaning |
  |---|---|
  | `91 87 14 06 25 80` | Confort permanent |
  | `91 87 14 07 25 80` | Réduit permanent |
  | `91 87 14 05 00 00` | Auto |
  | `91 87 14 05 23 00` | Auto + derogation |

  We write `00` and it works. The satellite may correct it; see section 5.
- **Boost** [high, issue #7 + official notice]:
  - The Connect raises the **comfort setpoint by 10 (+1.0 °C)** and sets **options bit `0x40`**:
    `91 87 1E 06 24 00` → `9B 87 1E 06 45 00`.
  - The satellite then reports room temperature, setpoint and mode `00 C3 00 CD 00 45` (setpoint 20.5 °C).
  - To stop, the Connect writes `…06 05`, then `05 01`.
  - The cloud stores `ACTIVITE_BOOST_Z1 = "360"`, which is the **6-hour limit in minutes**, and `DECALAGE_BOOST_Z1 = "10"`.
  - Official rules: +1 °C, at most 6 h, comfort only, cancelled at the next reduced period. It is not available on underfloor
    circuits, with outdoor-only regulation, on heat pumps, or on old satellites.
  - So boost is **managed by the Connect**: the Connect must restore the comfort setpoint itself when boost ends.
- The **options byte has the same format as the mode byte** that the satellite writes at `A031`. PR30's table: `0x20` Auto,
  `0x01` comfort, `0x02` derogation, `0x04` internal flag (fits our "meaningless on read"), `0x10` frost protection, `0x40` boost.
  Other tables (d33d33, Burnallover's wiki, Spanky) differ in details; the common ground matches ours.
- Schedule: 6 bytes per day, least significant bit first, half-hour bits, Sunday first. Confirmed by decoding the capture in issue
  #7 against the user's description. Cloud `plages[k]` = bit `k & 7` of byte `k >> 3`.

### 3.8 Other devices

- **Old 2011–15 satellites** (two Visio generations) read only `A033` (1 word) in their exchange, then read the date separately.
  Old satellites can only be read right after their "i" key is pressed [19814/230, 67032/724].
- At start-up and when "i" is pressed, the satellite sends **3 room-temperature-only frames** (`A02F 0001 02 <t>`), then `43 00 00`,
  then its normal 23-byte frame.
- **Hydroconfort**: the satellite repeats its frame for about 7 s before the boiler answers, and the answer is a **short 9-byte
  frame** with no zone data [erwannbst branch]. Our decoder should accept it.
- **Outdoor sensor**: `80 20 <assoc> <id> 01 17 9C 54 00 04 A0 29 00 01 02 <t:2>`, two's complement (`FF EC` = −2.0 °C). The
  boiler answers with the date. Daily at 04:00 the sensor sends `43 00 00` and writes `9C40 = 0000`.
- **Module H = address `25`** (hydraulic module for zones 2 and 3, optional wired outdoor sensor):
  - It forwards the outdoor temperature in the same format as the radio sensor.
  - **The boiler polls it**, with reception `03`: `25 80 .. 03 17 82 DD 00 01 A9 ED 00 02 04 00 CD 00 02` → `.. 83 17 02 00 E7`.
  - [medium, 19814/261, /321]
- **Address `84` = Frisquet heat pump (TEAMAO)**, sensor block at `7AA8` (the +0xC8 offset). One report of **`83`** for a 2015
  boiler fitted with the VisioLED kit. [high / low]
- **Fail-safe**: after about **20 min without satellite frames**, or 30 min without outdoor-sensor or Connect frames, the boiler
  goes into fault and **heating stops, frost protection included** [mgre 19814/65, /228, /365].
  - Relevant to the direct setpoint sent in the satellite's name (PROTOCOL-REFERENCE §9): impersonation must never become
    the only source of satellite frames and then stop.
  - Relevant to any future outdoor-sensor emulation (roadmap phase 3).

### 3.9 Anomalies (official documents)

- Visio boilers show **text messages, not numeric codes**.
- "Historique anomalies" keeps the last 15, each with its source, date and text. "Historique pression" keeps the minimum and
  maximum for each of the last 15 days. Neither has been found in radio memory.
- Messages related to the radio:
  - "Anomalie radio satellite Z1" (same family for the Module H, outdoor sensor, Frisquet Connect, pool box and solar tank)
  - "SATELLITE Z1 CHANGER PILES" (= the type `43` frame)
  - "AUCUN SATELLITE INITIALISE"
  - "Appareil incorrect"
  - "Anomalie liaison carte"
  - "FRISQUET CONNECT : SERVEUR INACCESSIBLE"
- Boiler messages: probe faults (sanitaire, corps de chauffe, départ, gaz brûlés), MANQUE D'EAU, PRESSION EAU TROP ELEVEE,
  SURCHAUFFE, TEMPÉRATURE GAZ BRÛLÉS, CIRCUIT AIR/GAZ, EVACUATION GAZ BRÛLÉS, VENTILATEUR, VANNE 4 VOIES, CAPTEUR DP, VANNE
  AUTO-ZERO, SECURITE BRULEUR, CONFIGURATION ERRONEE, CARTE ELECTRONIQUE, CAPTEUR PRESSION D'EAU [SAV p.44-45, installer notice
  p.35].
- The cloud only passes text labels (`alarmes[].nom`).
- **Implication**: a "last alert" text sensor fed by type `43` (and perhaps `45`) frames is the closest we can get.

### 3.10 Official behaviour worth copying

- **Holiday mode**: starts at 00:00 on the departure day and ends at midnight before the return day. Every zone uses its frost
  setpoint and hot water stops [user notice p.11, Connect notice p.13].
- **Consumption is estimated only on condensing boilers** [Connect notice p.9, p.15]. This explains our zeros, if the boiler is not
  a condensing model.
- Hot water: boilers without a tank only offer Max, Eco, Eco Horaires and Stop (no Eco+). Cloud mode id × 8 = `A0FC` value.
- Daily boiler self-check at **04:00 or 11:00** depending on the version ("Heure autocontrôle"). This may explain events seen at
  04:00.
- Changes made from the app reach the boiler and satellite in 1–10 min. The satellite resyncs its clock within 10 min after a
  battery change.
- The boiler wants a radio level of at least 3/5 (radio test). A 9 m antenna extension exists (F3AA41261).

### 3.11 Cloud API (for reference; not used by this firmware)

- Base URL `https://fcutappli.frisquet.com/api/v1`; orders are confirmed through a WebSocket at `wss://fcappcom.frisquet.com/`.
- Frisquet has threatened accounts that poll too often: 1 login and 1 consumption read per 24 h, site reads every 10–15 min at
  most.
- Variable names mirror the boiler: `TAMB`, `CAMB`, `CONS_CONF/RED/HG` (tenths of °C; radio value = cloud − 50), `SELECTEUR` 5–8
  (= A154 byte 3), `MODE` 6–8, `DERO`, `MODE_DERO` 0/6/7, `ACTIVITE_BOOST`, `MODE_ECS` id 0–5, `T_EXT`, `DATE_DEP/RET_VACANCES`,
  `identifiant_chaudiere` (14 digits, the first 4 being the year and week of manufacture).
- **Implication**: using the same names and enumerations for Home Assistant entities would keep us consistent with the official
  app.

### 3.12 Older product (not compatible)

The pre-2012 **Eco Radio System** (without Visio) is a different protocol:

- 433.42 MHz, OOK, differential Manchester with bit stuffing.
- A 17-byte frame `00 00 00 7E <id:2> 00 20 <n> <mode> <setpoint> 00 FE <chk:2> FF 80`, sent 3 times every 4 min.

Out of scope. Frisquet's documents call the Visio link "sécurisée et cryptée", but it is not encrypted.

---

## 4. Implications for the design

The modules below are the `components/frisquet/proto_*.h` headers.

1. **Clock (`proto_clock.h`)**: a 2-word read of `7A16` gives the boiler's local time, to the second, as one 32-bit value. It can
   cross-check the BCD date in `A029`/`A02B`. This is optional.
2. **Boiler pushes (engine / tracker)**:
   - Acknowledge `10` and `45` frames that arrive with reception `05`: addresses swapped, same id and association, reception `85`,
     same type. The data is empty for `45` and `<address:2> <count:2>` for `10`.
   - Today these would go unanswered. The boiler may retry them or raise an anomaly, as it does for zone relays.
   - Decode a `10` on `A0F0` as an update of the DHW mode, holiday and date block.
3. **Decoder (`proto_decode.h`)**: name the Modbus exception codes; decode `42`, `43` (text), `44` and `45`; accept the 9-byte short
   answer from Hydroconfort boilers.
4. **Alerts**: a "last alert" text sensor fed by `43` frames (low satellite battery) and `45` events (device, code, time). Clear it
   on `43 00 00`.
5. **Boost (`proto_zone_store.h` / `proto_schedule.h`)**: a per-zone boost switch.
   - Start: write comfort + 10 and set `0x40`, from Confort or Auto in a comfort period only.
   - Stop: restore the stored comfort setpoint and clear `0x40`, after 6 h or on request.
   - Persist the original setpoint, because the Connect owns the timer.
   - Not available on underfloor circuits or with outdoor-only regulation.
6. **Holiday**: the firmware exposes a start/end date-time range and an explicit activate/cancel action.
   The `A0F0` partial write changes eight words, then reads `A0F0 × 0D` back for verification.
   - Future programming, cancellation, autonomous start and expiry verified on the real boiler on 2026-11-11.
     The boiler pushed cleared dates at expiry; the satellite setpoint went 18 → 8 → 18 °C without changing the saved
     Auto mode, weekly program or temperatures. Satellite effects lagged by up to two 10-minute exchanges.
   - Still to characterize: DST interpretation, other boiler versions and actual DHW production during the period.
   - The partial write is intended to leave the remaining `A0F0` words (`A0F8` date, `A0FC` DHW mode, serial) untouched.
7. **Zone byte 5**: keep "copy from the last relay". If tests show `0x80` is required for permanent Confort or Réduit, set it
   when writing modes `06` and `07` and clear it for `05`.
8. **Diagnostics sensors**: panel version (`7931`/`7541`), versions of the paired devices (`7A30–7A33`), flow target (`A033`),
   flow minimum and maximum (`7946`), serial (end of `A0F0`), and the date at `A0F8`.
9. **Pairing (`proto_pairing.h`)**: make the confirmation payload configurable (NetworkID echo, which works today, or the version
   `01 21 01 02`) for experiment 6.4.
10. **Satellite impersonation safety**: never let the direct setpoint in the satellite's name replace the real satellite's
    frames. If the real satellite falls silent for about 20 min, the boiler stops heating.
11. **Radio**: try `setRxBoostedGainMode(true)` and a 5-byte TX preamble. Both must stay compatible with the bit-slip repair
    and the fixed 68-byte capture.

---

## 5. Contradictions with our verified facts (resolve before relying on them)

| Topic | Ours | Theirs | How to settle |
|---|---|---|---|
| Anomaly bit in `A02E` | bit 0 (`0x29` with "anomalie avec Connect", DESIGN 10.3) | `0x80` = anomaly [Freedom] | Probably two flags: radio partner lost (bit 0) and boiler fault (`0x80`). Log `A02E` during any real fault. |
| `0x20` in `A02E` | standby (`20`) versus demand (`28`) | `0x10`/`0x20` = Connect present, `0x04` = dry contact | Watch whether `0x20` drops after a long Connect silence (with care: the anomaly appears after about 11 h). |
| Type `43` | "SONDE_INIT" (frisquet-bridge) | text alert | Settled: text alert (captures). |
| Zone byte 5 | `00`; "never modify it" | `0x80` = permanent mode [PR30] | Experiment 6.8. |
| Preamble | 16 bits | 5 × `0xAA` on air | Both work for receiving; see 6.1 for sending. |
| Weekday | 0 = Sunday [own data] | d33d33 uses Sunday = 7 in one place | Ours is verified. |
| Power words | — | kW versus 0.1 kW | Compare with the flame state on our boiler. |
| Pressure | ÷ 5120 | ÷ 5120 confirmed (2.2 bar = `2C00`); one user computes 1.6 bar while the panel shows 1.2–1.3 | Compare with the panel display. |

---

## 6. Experiments, cheapest and most useful first

1. **RX gain and preamble**: `setRxBoostedGainMode(true)`, then a 5-byte TX preamble. Measure whether satellite frames are received
   (the goal) and whether boiler frame reception and the bit-slip repair are unchanged.
2. **Scan `7531–7544`** word by word, with `dev/ofv_scan.py --split`.
3. **Ask the user** what service date the boiler shows; compare with 2024-09 at `A0F8`.
4. **Pairing with a version**: re-pair through "Remplacer › Frisquet Connect", answering `01 21 01 02`. Then read `7A30–7A33` and
   watch for `10`/`45` pushes, acknowledging them with `85`.
5. **Change something on the boiler panel** (DHW mode, holiday) and watch for a type-`10` push on `A0F0` with reception `05`.
6. **Boost**: on zone 1 in Confort, write comfort + 10 with options `0x45`. Check the relay (`0x40`), `A029` (setpoint + 1.0) and
   the satellite display ("BOOST"). Then stop it.
7. **Holiday**: write `A0F0` start and end for a short future window, read it back, and check that the zones go to frost protection
   and the DHW mode changes. Cancel by writing zeros.
8. **Byte 5**: set Confort permanent locally on the satellite and check whether its relay carries `80` at byte 5.
9. Read **`A02E` and `9C40`** around 04:00 and during any fault, and log every `43`/`45`/`42` frame.

---

## 7. Still unknown after research

- Where the anomaly history and pressure history are stored.
- Where the heating-curve slope is stored; the rest of `793E–7943` and `7954–7958`.
- Meaning of type `44`, and the `45` code table.
- Frisquet Connect hardware, its link to the cloud and its firmware: nothing published.
- Regulatory filings for the satellite or the Connect: none found.
- Frisquet patents: only 5 since 1995, none about radio.
