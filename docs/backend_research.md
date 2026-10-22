# Backend research notes: ESP32 web dashboard

Notes from looking into how the backend for the FlyPrecise dashboard could work.
This is what I found out while researching the ESP32 and testing ideas, not a
final design. The front end (the HTML/CSS in `frontend/`) is done; this covers
what the backend that serves those pages would need to do and what we learned
about the ESP32 along the way.

## The basic idea

- The Uno stays in charge of the rig (pump, valves, flow meter, density sensor,
  LCD). Nothing about that changes.
- An ESP32 sits next to it and does one job: run a Wi-Fi hotspot and serve the
  dashboard pages to a phone or laptop.
- The two boards talk over a plain serial (UART) line. The Uno sends readings,
  the ESP32 sends start/stop.
- No internet needed. The ESP32 makes its own network (something like
  "FlyPrecise" at 192.168.4.1) and the phone just joins it.
- No JavaScript. Every page is built on the ESP32 and sent as finished HTML.
  The live numbers update because the small live panel reloads itself once a
  second with a `<meta refresh>` tag, so the rest of the page stays still.

## What we found out about the ESP32

**Which chip.** The ESP32-S3-MINI-1 module (N8 version, 8 MB flash) is the
one that fits. It has 2.4 GHz Wi-Fi, native USB for programming, and plenty of
room for the web pages and a log file. The ESP32-S3-DevKitM-1 is the same
module on a dev board with its own regulator and USB-C, which is the easy way
to get started before the PCB exists.

**Power.** It cannot run off the Uno's 3.3 V pin. That pin gives about 50 mA
and the ESP32 pulls 300 to 500 mA in bursts when Wi-Fi transmits. It has to
come off the 5 V rail with its own regulator (the DevKit has one built in).
A big cap (around 100 uF) near the ESP32 helps with the bursts.

**3.3 V logic, not 5 V.** The ESP32's pins are not 5 V tolerant. The Uno's TX
line is 5 V, so it needs a resistor divider (1 k / 2 k) before it reaches the
ESP32's RX pin. Going the other way, 3.3 V from the ESP32 is high enough for
the Uno to read as a 1, so that direction works directly.

**Which Uno pins to use.** We looked at SoftwareSerial on spare pins (D7/D13)
so the USB port stays free. Problem: SoftwareSerial turns off interrupts for
about a millisecond per byte, and the flow meter counts pulses with an
interrupt on D2. We would lose pulses while talking to the ESP32, which is bad
for a project that is all about accuracy. So the link should use the hardware
serial pins D0/D1 that the USB port already uses. The ESP32 just ignores any
line that doesn't start with `$`, so debug prints and protocol lines can share
the same wire.

**Sharing D0 with USB has a catch.** The common trick is a 1 k resistor between
the ESP32 TX and D0 so you can still upload sketches to the Uno without
unplugging the ESP32. But the Uno already has a 1 k resistor on that line from
its own USB chip. With both, the line sits at about 2.5 V when the ESP32 pulls
low, and the Uno might not see it as a 0. It works fine with USB unplugged,
which is why it is easy to miss on the bench. A diode plus a pull-up resistor
instead of the plain resistor fixes it. Worth deciding before the shield is
ordered.

**ESP32 pins.** GPIO17 (TX) and GPIO18 (RX) are free and safe to use for the
UART. Pins 0, 3, 45, 46 are "strapping pins" that affect boot mode and should
be left alone.

**Antenna.** If the bare module goes on the PCB, the antenna end has to hang
over the board edge or sit over a copper-free area. Putting it in the middle of
a ground pour kills the range.

**Phones and offline hotspots.** iPhones open a hotspot with no internet in a
restricted "sign in" window with no address bar and no add-to-home-screen. The
workaround is to tap Cancel, choose "Use Without Internet", and open Safari at
192.168.4.1. Android does something similar. Not a bug, just something for the
demo script.

**Time.** The ESP32 has no battery clock and no internet for NTP, so it
doesn't know the date on boot. The simplest fix is a settings page where you
type the date and time once; it keeps time from there with `millis()`.

## How the serial link would work

Simple text lines at 9600 baud so the Arduino serial monitor doubles as a
debugger.

Uno to ESP32, twice a second:

```
$T,<phase>,<density>,<mL>,<grams>,<target_g>,<tof_mm>,<flags>
```

Uno to ESP32, once when it happens:

```
$E,START,<target_g>
$E,STOP,<mL>,<grams>,<density>,<TARGET|MANUAL|ERROR>
$E,ERR,<text>
$E,BOOT
```

ESP32 to Uno:

```
R<kg>     set target and start
S         stop
P B M     manual pump / bypass / mainline toggles
```

Density goes over the wire in g/mL since that's what the Uno computes. The
ESP32 converts to lb/gal for the display. The `$E,STOP` line is what becomes a
row in the history table and a fuel slip.

## What the ESP32 would store

Two small files on its flash (LittleFS):

- `sessions.csv`: one row per fill (id, time, target, delivered volume,
  delivered mass, density, how it ended). This feeds the PAST FLIGHTS tab,
  the ANALYTICS tab, and each fuel slip.
- `settings.json`: units, planning density, calibration coefficients, hotspot
  password, next record id.

At about 60 bytes a row, 1.5 MB of space holds way more fills than we will
ever do.

## Things in the current Uno code the backend would run into

Found these while reading `DemoFinal.ino`:

- The auto-stop check compares grams to kilograms, so the condition is
  basically always true. Should be confirmed against what the beta demo did.
- Volume and mass totals never reset between fills. A slip needs per-fill
  numbers, so they would have to zero at each start.
- The ToF sensor read can block for up to 500 ms if the sensor glitches,
  which would delay a STOP from the phone. Checking a data-ready flag first
  avoids that.
- If the ToF sensor fails to start, the sketch halts forever. Better to flag
  the error and keep the pump controllable.
- The `R` command waits in a blocking loop for the number. It should read the
  number off the same line instead.

## Where this leaves things

- Front end: finished, in `frontend/`, with a README that lists every value
  the backend fills in and every form field it receives.
- A reference ESP32 build that renders those pages exists in
  `esp32-dashboard/`. It compiles but has not been flashed to a board or
  connected to the rig yet.
- Still to do: get a DevKit, flash it, test the serial link on a breadboard
  with the Uno, then try it on the rig.
- The detailed plan and the review notes are in this folder too
  (`FLYPRECISE_DASHBOARD_PLAN.md`, `plan_review.md`, `serial_protocol.md`).
