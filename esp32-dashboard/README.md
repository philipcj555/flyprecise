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

Board target: `esp32-s3-devkitm-1`. The shield is laid out for both a plug-in
DevKitM-1 and a bare ESP32-S3-MINI-1-N8 (same module), so the build target and
pins are identical either way. Flashing and the debug log go over the module's
native USB (GPIO19/20): the DevKit's port labelled **USB**, or the USB-C
connector on the shield for the bare module.
First flash: hold BOOT, tap RESET, release BOOT, then `pio run -t upload`.
After that esptool resets the board itself.

## Connect

1. Join Wi-Fi `FlyPrecise`, password `flyprecise` (change it in `include/config.h`
   or from the Settings page; a changed password applies after reboot).
2. The dashboard should pop up as a captive portal. If not, open
   `http://192.168.4.1/` (or `http://flyprecise.local/`).
3. Set the clock on the Settings page once per power-up so slips get a date.

## Pages

Layout follows the team sketch: three tabs, NOW / PAST FLIGHTS / ANALYTICS.

| path            | what |
|-----------------|------|
| `/`             | NOW: flight line, fuel requested, live density, gallons loaded, FlyPrecise difference (iframe refreshing every 1 s), load-a-flight form, START, STOP |
| `/live`         | the refreshing panel itself |
| `/past`         | last 50 flights, newest first (`/history` is an alias) |
| `/analytics`    | totals, average density, per-flight difference chart |
| `/slip?id=N`    | printable fuel slip |
| `/export.csv`   | download the flights CSV |
| `/settings`     | clock, units, planning density, calibration coefficients, AP password |
| `/advanced`     | manual pump / valve toggles (not in the nav) |
| `/debug`        | plain-text state dump for bench work |

**FlyPrecise difference** = fuel requested x (1 - measured density / planning
density). Planning density defaults to 6.7 lbs/gallon (Settings). Negative means
the aircraft carries less fuel than a density-assumed load for the same requested
mass. With the sketch's numbers (245,850 lbs requested, 6.91 measured) it gives
-7,712 lbs.

## Storage

`sessions.csv` (append-only, rotates to `sessions.1.csv` past ~300 KB):

```
id,epoch,flight,origin,dest,aircraft,target_g,delivered_ml,delivered_g,density_gml,reason
1,1762112345,XX1234,LAX,SIN,B787-900,3000,2940,2396,0.815,TARGET
```

`epoch` is 0 if the clock had not been set when the fill ended. Times are stored
as the wall-clock value entered on the Settings page (no time zone).

`settings.json`: `{"next_id":2,"units_mass":"lb","units_vol":"gal","nominal_lbgal":6.7,"cal_a":1,"cal_b":0,"ap_pass":""}`

## Layout

```
platformio.ini        two envs: devkit (real), fake (FAKE_UNO simulator)
include/config.h      pins, SSID, limits, paths
src/main.cpp          AP, captive DNS, mDNS, main loop
src/uno_link.*        $T/$E parser, command sender, FAKE_UNO simulator
src/store.*           LittleFS: sessions.csv, settings.json, clock
src/web.*             routes and server-side HTML
data/                 style.css, manifest.json, icon.svg (embedded at build)
uno/Demo_v3.ino       reference Uno sketch that speaks the protocol (DemoFinal + $T/$E)
```
