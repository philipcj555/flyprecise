# FlyPrecise ESP32 dashboard

Wi-Fi front end for the FlyPrecise rig. The ESP32 hosts an access point and a
pure HTML + CSS dashboard (no JavaScript). The Uno keeps all control logic.

Spec: `../docs/FLYPRECISE_DASHBOARD_PLAN.md`. Protocol: `../docs/serial_protocol.md`.

## Build / flash

Needs [PlatformIO](https://platformio.org) (`pip install platformio` or the VS Code extension).

```
cd esp32-dashboard
pio run -e fake -t upload       # simulator build: full UI, no Uno needed
pio run -e devkit -t upload     # real build: talks to the Uno on GPIO17/18
pio device monitor              # debug log over the DevKit's native USB port
```

No filesystem upload step. `data/style.css`, `manifest.json` and `icon.svg` are
embedded into the firmware at build time. LittleFS is formatted on first boot and
holds only `sessions.csv` and `settings.json`.

Board: `esp32-s3-devkitm-1` (ESP32-S3-MINI-1-N8, 8 MB flash, no PSRAM). Plug the
USB cable into the port labelled **USB** (native), not **UART**, for the debug log.

## Connect

1. Join Wi-Fi `FlyPrecise`, password `flyprecise` (change it in `include/config.h`
   or from the Settings page; a changed password applies after reboot).
2. The dashboard should pop up as a captive portal. If not, open
   `http://192.168.4.1/` (or `http://flyprecise.local/`).
3. Set the clock on the Settings page once per power-up so slips get a date.

## Pages

| path            | what |
|-----------------|------|
| `/`             | live panel (iframe refreshing every 1 s), target entry, START, STOP |
| `/live`         | the refreshing panel itself |
| `/history`      | last 50 sessions, newest first |
| `/slip?id=N`    | printable fuel slip |
| `/export.csv`   | download `sessions.csv` |
| `/settings`     | clock, units, calibration coefficients, AP password |
| `/advanced`     | manual pump / valve toggles (not in the nav) |
| `/debug`        | plain-text state dump for bench work |

## Storage

`sessions.csv` (append-only, rotates to `sessions.1.csv` past ~300 KB):

```
id,epoch,target_g,delivered_ml,delivered_g,density_gml,reason
1,1762112345,3000,2940,2396,0.815,TARGET
```

`epoch` is 0 if the clock had not been set when the fill ended. Times are stored
as the wall-clock value entered on the Settings page (no time zone).

`settings.json`: `{"next_id":2,"units_mass":"kg","units_vol":"L","cal_a":1,"cal_b":0,"ap_pass":""}`

## Layout

```
platformio.ini        two envs: devkit (real), fake (FAKE_UNO simulator)
include/config.h      pins, SSID, limits, paths
src/main.cpp          AP, captive DNS, mDNS, main loop
src/uno_link.*        $T/$E parser, command sender, FAKE_UNO simulator
src/store.*           LittleFS: sessions.csv, settings.json, clock
src/web.*             routes and server-side HTML
data/                 style.css, manifest.json, icon.svg (embedded at build)
```
