# FlyPrecise Web Dashboard Plan

Spec for the ESP32-hosted dashboard. Written for Claude Code to build from. Repo: github.com/nklloyd/flyprecise

## 1. Hard constraints (do not violate)

1. **No JavaScript anywhere in the browser.** Pure HTML + CSS only. No inline scripts, no `onclick`, no fetch, no WebSockets, no service workers. A `manifest.json` for home-screen install is fine (it's JSON, not JS).
2. **The Elegoo Uno R3 stays the brain.** It owns sensors, the fill state machine, valves, pump, and the 16x2 LCD. The ESP32 is a Wi-Fi front end only. Do not move control logic to the ESP32.
3. **Must work with zero internet.** Demo location may have no Wi-Fi. ESP32 runs its own access point.
4. **Target board: ESP32-S3-MINI-1 module** (3.3 V logic, 2.4 GHz Wi-Fi, native USB, 8 MB flash on the N8 variant, no PSRAM). Confirm the actual part number on the module: `ESP32-S3-MINI-1-N8` is the common one.
5. **Only include features the team has actually built** in any submission docs. Don't describe things in this plan as done until they are.

## 2. What exists today (as of Oct 15, 2026)

- `Demo/DemoFinal.ino` is the working Uno firmware. Pins in use:
  - D2 flow meter interrupt (Hall effect, FALLING)
  - D3, D4, D5, D8, D11, D12 LCD (LiquidCrystal, 4-bit)
  - D6 pump MOSFET, D9 bypass valve MOSFET, D10 mainline valve MOSFET
  - A4/A5 I2C for VL53L0X ToF sensor
  - Free: D0/D1 (USB serial), D7, D13, A0, A1, A2, A3
- Fill sequence: IDLE -> PRIMING (10 s, bypass open, pump on) -> OVERLAP (mainline open, 150 ms) -> RUNNING -> auto stop at `totalGrams >= target - 50`.
- Serial commands already exist: `S` stop, `P` pump toggle, `B` bypass toggle, `M` mainline toggle, `R` then a float = set target kg and start.
- Density is a hardcoded `1.02` placeholder. Linear calibration (corrected = a*raw + b) is the next sensor task, due Oct 19.
- LCD shows `D: x.xx  L: x.xx` / `kg: x.xx`.
- Custom KiCad shield PCB is in progress (`PCB/ArduinoShield`). **Not ordered yet.** The ESP32 footprint and support parts below must go on it before ordering.
- Beta deck promised: ESP32 Wi-Fi, live readings on a web dashboard, target mass entry, digital fuel slip. Scheduled Nov 2 to 16.

## 3. Architecture

```
 phone / laptop
      |  Wi-Fi (ESP32 SoftAP "FlyPrecise", open 192.168.4.1)
      v
 ESP32-S3-MINI-1  (C++ / Arduino framework)
   - HTTP server, renders HTML server-side
   - LittleFS: sessions.csv, settings.json
   - parses telemetry lines from Uno
   - forwards start/stop/target commands to Uno
      ^
      |  UART, 9600 baud, 3.3V/5V level handled in hardware
      v
 Elegoo Uno R3  (existing DemoFinal.ino + small changes)
   - sensors, state machine, MOSFETs, LCD
```

## 4. Hardware integration

### 4.1 Module vs dev board

Decide this first. Two paths. **Team decision Oct 15: path B.**

- **A. ESP32-S3-DevKitM-1** (dev board that carries the MINI-1). Has its own 3.3 V regulator, USB-C, and headers. Easiest for Nov. Mount it off the shield with a 4-pin header (5V, GND, TX, RX). Recommended for this semester.
- **B. Bare ESP32-S3-MINI-1 soldered to the shield.** Cleaner final product but needs: 3.3 V LDO, boot/reset buttons or pads, USB-C connector or a 6-pin programming header, and reflow or careful hand soldering of the castellated module. Only do this if someone on the team has done castellated modules before and the PCB isn't already late.

Everything below assumes A unless noted.

### 4.2 Power

- **Do not power the ESP32 from the Uno's 3V3 pin.** That regulator is good for ~50 mA. Wi-Fi TX bursts pull 300 to 500 mA.
- Power from the shield's 5 V rail into the DevKit's `5V` pin (its onboard LDO handles 3.3 V). Add a 100 uF cap near the header.
- If path B: AMS1117-3.3 or better (AP2112K-3.3 / AP7361 for lower dropout), 10 uF in + 10 uF out, rated >= 600 mA.
- Common ground between Uno, ESP32, MOSFET board, and the 12 V valve supply. Already required for the MOSFETs, just make sure the ESP shares it.

### 4.3 UART wiring and the level shift

ESP32-S3 GPIOs are **not 5 V tolerant**. Uno TX is 5 V.

- Uno TX -> ESP32 RX: resistor divider, 1 kΩ series + 2 kΩ to GND (5 V -> 3.33 V). Or a dedicated level shifter breakout.
- ESP32 TX -> Uno RX: 3.3 V HIGH is above the ATmega328's 3.0 V Vih threshold, works directly. Add a 1 kΩ series resistor (see 4.4).
- Use **ESP32 GPIO17 (TX) and GPIO18 (RX)** as UART1 (any GPIO works on S3 via the pin matrix; these are free on the DevKitM-1). Avoid strapping pins 0, 3, 45, 46.

### 4.4 Which Uno pins to use for the link (important, read this)

Two options. **Use option 2.**

**Option 1: SoftwareSerial on D7 (RX) / D13 (TX).** Keeps USB serial free for debugging. Problem: SoftwareSerial turns interrupts off for roughly 1 ms per byte while sending or receiving. The flow meter pulse counter on D2 is interrupt-driven. At realistic flow rates (50 to 150 Hz pulses) a 30-byte telemetry line every 500 ms gives a few percent chance per pulse of being missed. For a system whose whole pitch is precision, that's a real error source. Rejected.

**Option 2: Hardware Serial on D0/D1, shared with USB.** No interrupt blackout, zero flow meter risk. Wiring:
- Uno D1 (TX) -> divider -> ESP32 RX. The PC also sees these lines over USB, which is fine for debugging.
- ESP32 TX -> **1 kΩ series resistor** -> Uno D0 (RX). The series resistor lets the Uno's USB-serial chip win when you upload a sketch, so you don't have to unplug the ESP32 to program the Uno. Standard trick.
- Firmware consequence: Uno `Serial` carries both protocol lines and any debug prints. The ESP32 ignores any line that doesn't start with `$`. Strip the noisy `LOOP BEGAN` / `COMMAND UPDATED` prints from DemoFinal.ino or they'll flood both links.

### 4.5 Add to the shield PCB before ordering

1. 4-pin header for the DevKit (5V, GND, UNO_TX, UNO_RX), labeled on silkscreen.
2. Divider resistors (1k / 2k) on the UNO_TX line.
3. 1 kΩ series resistor on the ESP_TX -> D0 line.
4. 100 uF electrolytic on 5 V near the header.
5. A jumper or solder bridge on the ESP_TX -> D0 line so it can be cut if uploads ever fight.
6. Test points on UNO_TX, UNO_RX, 5V, 3V3 (if path B), GND.
7. Net labels per Nick's 10/14 comments: `ESP_RX`, `ESP_TX`, not pin numbers.

## 5. Serial protocol (Uno <-> ESP32)

9600 baud, 8N1, newline terminated, ASCII. Keep it human-readable so the Arduino serial monitor doubles as a debugger.

### 5.1 Uno -> ESP32 telemetry, every 500 ms

```
$T,<phase>,<dens>,<ml>,<g>,<target_g>,<dist_mm>,<flags>
```
- `phase`: `IDLE` | `PRIMING` | `OVERLAP` | `RUNNING`
- `dens`: density in lbs/gal (float, 3 decimals) after calibration; `-1` if not valid yet
- `ml`: totalMilliLitres (uint)
- `g`: totalGrams (uint)
- `target_g`: current target in grams (uint)
- `dist_mm`: raw ToF reading (uint)
- `flags`: bitfield as int. bit0 = pump on, bit1 = bypass open, bit2 = mainline open, bit3 = ToF timeout, bit4 = sensor init error

Example: `$T,RUNNING,6.812,1830,1867,3000,142,5`

### 5.2 Uno -> ESP32 events (sent once when they happen)

```
$E,START,<target_g>
$E,STOP,<ml>,<g>,<dens>,<reason>     reason: TARGET | MANUAL | ERROR
$E,ERR,<text>
$E,BOOT
```
`$E,STOP` is what the ESP32 turns into a session row and a fuel slip.

### 5.3 ESP32 -> Uno commands

Reuse what exists, make them line-based:
```
R<kg>\n     set target and start (e.g. R2.5)
S\n         stop
P\n B\n M\n manual toggles (only shown on an "advanced" page)
C<a>,<b>\n  set density calibration coefficients (optional, phase 2)
```

### 5.4 Uno firmware changes

1. Remove debug `Serial.println` spam in `loop()` and `updateDistance()`.
2. Add `sendTelemetry()` on a 500 ms timer and `sendEvent()` calls in `startSystem()` / `stopSystem()`.
3. Make `R` non-blocking: parse the float from the same line instead of the current `while(Serial.available()==0){}` wait. Reject if not IDLE.
4. Replace the hardcoded `1.02` with the calibrated density function once the Oct 19 calibration is done. Until then send `dens = -1` and the page shows `--`.
5. Keep the LCD working exactly as is. The dashboard is additive.

## 6. ESP32 firmware

Arduino framework (PlatformIO preferred so Claude Code can build/flash from CLI; Arduino IDE 2 also fine). Board: `esp32-s3-devkitm-1`. Partition scheme with a LittleFS region of at least 1.5 MB.

### 6.1 Startup

1. Mount LittleFS. If missing, format and create `settings.json` and `sessions.csv` with header.
2. `WiFi.softAP("FlyPrecise", <password>)`. Use a password so random phones don't connect. Static IP 192.168.4.1. Optional: mDNS `flyprecise.local`.
3. Run a captive-portal DNS (DNSServer) so connecting to the hotspot pops the page open on most phones. Pure server-side, no JS needed.
4. `Serial1.begin(9600, SERIAL_8N1, 18, 17)`.
5. Start HTTP server on port 80.

### 6.2 State kept in RAM

Latest telemetry struct, last event, uptime, "clock set" flag + epoch offset, current session in progress.

### 6.3 Routes (all HTML is generated server-side)

| Route | Method | Purpose |
|---|---|---|
| `/` | GET | Shell page: header, `<iframe src="/live">`, target form, stop form, nav |
| `/live` | GET | Tiny page with `<meta http-equiv="refresh" content="1">`. Big readouts: density, volume, mass, phase pill, valve/pump indicators, warning if ToF timeout. This is the only thing that reloads. |
| `/start` | POST | form field `kg`. Validate 0.1 to 50. Send `R<kg>\n`. `303 See Other` back to `/`. |
| `/stop` | POST | Send `S\n`. Redirect `/`. |
| `/history` | GET | Table of sessions from CSV, newest first. Each row links to its slip. |
| `/slip?id=N` | GET | Fuel slip page: session #, date/time, target, delivered volume, density, delivered mass, reason, blank lines for aircraft tail #, pilot, signature. Print stylesheet. |
| `/export.csv` | GET | Raw `sessions.csv` download, `Content-Disposition: attachment`. |
| `/settings` | GET/POST | Set clock (`<input type="datetime-local">`), units (kg/lb, L/gal), optional cal coefficients a and b. Saves to `settings.json`. |
| `/advanced` | GET/POST | Manual P/B/M toggles with a warning banner. Hidden from main nav. |
| `/manifest.json` | GET | PWA manifest (name, icons, `display: standalone`, theme color). |
| `/style.css` | GET | Single stylesheet, served with cache headers. |
| `/icon.svg` | GET | App icon. |

Any unknown path -> redirect to `/` (keeps the captive portal happy).

### 6.4 Storage format

`sessions.csv`:
```
id,epoch,target_g,delivered_ml,delivered_g,density_lbgal,reason
1,1762112345,3000,2940,2951,6.81,TARGET
```
`id` is a counter persisted in `settings.json`. If clock was never set this boot, `epoch` is 0 and the slip shows "Session 14 (time not set)" instead of a date.

`settings.json`: `{ "next_id": 15, "units_mass": "kg", "units_vol": "L", "cal_a": 1.0, "cal_b": 0.0, "ap_pass": "..." }`

Append-only writes. Cap the CSV at ~5,000 rows; if exceeded, rotate to `sessions.1.csv`.

### 6.5 Time

No battery RTC, no NTP without internet. On each boot the `/live` page shows a yellow "Clock not set" banner linking to `/settings` until someone submits the datetime form. ESP32 then tracks time from `millis()` offset. Good enough for slips.

## 7. UI / look

Goal: looks like a purpose-built instrument panel, not a default HTML form. All CSS.

1. **Dark theme**, near-black background, one accent color (aviation amber or a cool blue), high-contrast white text. Define colors as CSS variables.
2. **Readouts**: density, volume, mass as large monospace numbers (`font-variant-numeric: tabular-nums` so digits don't jitter between refreshes), unit label small beneath each.
3. **Phase pill**: `IDLE` gray, `PRIMING` amber, `OVERLAP` amber, `RUNNING` green with a CSS keyframe pulse, `ERROR` red. Three small dots for pump / bypass / mainline state.
4. **Forms**: one big number input + one big START button. STOP is a separate full-width red button, always visible. `<input type="number" min="0.1" max="50" step="0.1" required>` gives validation without JS.
5. **Mobile-first**, single column, fine on a laptop too. 16 px side gutters, no horizontal scroll.
6. **Slip page**: white background, ticket-style layout, `@media print` hides nav and sets a 80 mm width option for receipt printers plus a letter-size fallback.
7. **PWA manifest** so it can be added to a home screen with the FlyPrecise icon and no browser chrome.
8. System font stack. No web fonts (no internet).

## 8. Build order for Claude Code

Do these in sequence; each step is testable on its own.

1. **ESP32 skeleton**: PlatformIO project, SoftAP, captive DNS, LittleFS mount, `/` returning "hello". Flash it, connect a phone, confirm the page loads.
2. **Fake telemetry mode**: compile flag `FAKE_UNO` that generates a simulated fill (density 6.8, flow ramps) so the whole UI can be built without the rig. Build `/live`, `/`, `/style.css`, `/manifest.json`. Get the look right here.
3. **Forms**: `/start`, `/stop`, redirects, validation, fake mode responds to them.
4. **Storage**: `$E,STOP` -> CSV row, `/history`, `/slip`, `/export.csv`, `/settings` with clock.
5. **Uno side**: create `Demo/Demo_v3/Demo_v3.ino` from DemoFinal.ino with the protocol changes in 5.4. Test with the Arduino serial monitor by hand before connecting the ESP32.
6. **Bench integration**: Uno + ESP32 on a breadboard with the divider and 1 kΩ resistor, no pump. Confirm telemetry parses and `R2.5` starts the sequence.
7. **Rig test**: full system, water, 3 L target. Compare slip volume to the flow meter's own display and a measured container.
8. **Polish**: error banners, ToF timeout display, units toggle, `/advanced`.

Repo layout:
```
esp32-dashboard/
  platformio.ini
  src/main.cpp
  src/web.cpp / web.h          routes + HTML rendering
  src/uno_link.cpp / .h        parser + command sender
  src/store.cpp / .h           LittleFS CSV/JSON
  data/style.css, manifest.json, icon.svg
Demo/Demo_v3/Demo_v3.ino
docs/serial_protocol.md         copy section 5 here
```

## 9. Testing checklist

- [ ] Phone with no internet connects to `FlyPrecise` AP and page auto-opens or loads at 192.168.4.1
- [ ] `/live` refreshes at 1 s without the rest of the page flashing
- [ ] Entering 2.5 and pressing START makes the Uno print `$E,START,2500` and the LCD switch to pumping
- [ ] STOP works mid-fill from the phone
- [ ] Session appears in `/history` with correct numbers after auto-stop
- [ ] Slip prints cleanly from phone (Safari share -> print) and from laptop (Ctrl+P)
- [ ] Uploading a new Uno sketch works with the ESP32 still wired
- [ ] Flow meter total with ESP32 attached matches total with ESP32 unplugged (proves no interrupt loss)
- [ ] Power-cycle: sessions survive, clock banner reappears
- [ ] Two phones connected at once both see live data
- [ ] Page loads in under 1 s on the ESP32 (keep each HTML response under ~8 KB)

## 10. Timeline fit

- **Now to Oct 19**: nothing here blocks density calibration. Parallel: order DevKitM-1 (and a second as a spare), add header + resistors to the shield schematic.
- **Oct 19 to Nov 2**: PCB finalize/order. Steps 1 to 4 above can run entirely in fake mode on a DevKit without the rig.
- **Nov 2 to 16**: steps 5 to 8, matches the beta deck's Wi-Fi + dashboard block.
- **Nov 16 to Dec 4**: accuracy testing, use `/export.csv` to pull data into the final report.

## 11. Open questions for the team

1. DevKitM-1 off-board (path A) or bare module on the shield (path B)? Default A.
2. AP password? Needs to be in the final docs for graders.
3. Units on the slip: kg + L (SI, what the Uno uses now) or lb + gal (what the deck quotes density in)? Probably show both.
4. Does anyone want BLE? (No. Wi-Fi AP covers it. Just confirming.)
5. Is the "no JavaScript" rule browser-only, or also "no Python helper scripts"? Affects how the calibration fit is computed.
