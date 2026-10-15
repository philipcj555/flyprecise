# Review notes on FLYPRECISE_DASHBOARD_PLAN.md

Written Oct 15, 2026 while building steps 1 to 5 of the plan. Ordered by how
much they matter. Items marked **PCB** need a decision before the shield is ordered.

## 1. ESP32 TX -> Uno D0 through a 1 kΩ resistor will not work reliably (PCB)

The Uno already has a 1 kΩ series resistor between its USB-serial chip
(ATmega16U2) and D0. When USB is plugged in, the 16U2 holds its TX idle HIGH at
5 V. Adding a second 1 kΩ from the ESP32 makes the two outputs an even divider:
when the ESP32 drives LOW, D0 sits at about 2.5 V. The ATmega328P needs below
1.5 V for a LOW, so commands from the ESP32 (START / STOP) can be missed
whenever a laptop is connected for debugging. Without USB the 16U2 is high-Z
and it works, which is why this is easy to miss on the bench.

Suggested fix on the shield: ESP32 TX -> Schottky diode (cathode on the ESP32
side, e.g. BAT54) -> D0, plus a 10 kΩ pull-up from D0 to 5 V. The ESP32 can
then only pull the line LOW, the pull-up or the 16U2 provides HIGH, and
uploads never fight the ESP32. Keep the cuttable jumper in series as the plan
already says.

## 1b. Path B chosen (bare module on the shield): what the shield must carry (PCB)

Decision Oct 15: the ESP32-S3-MINI-1-N8 goes directly on the shield (plan
section 4.1 path B). Firmware is unchanged (the DevKitM-1 is the same module),
but the shield now has to provide everything the DevKit did:

1. **3.3 V regulator** from the 5 V rail, rated 600 mA or more with headroom
   for Wi-Fi bursts: AP2112K-3.3 or AP7361-33 preferred (low dropout), AMS1117-3.3
   acceptable. 10 uF ceramic in and out, plus 100 nF right at the module's 3V3 pin
   and 22 uF bulk nearby.
2. **EN (CHIP_PU)**: 10 kΩ pull-up to 3.3 V, 1 uF to GND, and a RESET push button
   to GND. Without the RC the module may not start cleanly on power-up.
3. **GPIO0 BOOT button** to GND (internal pull-up is enough). Needed to enter
   the bootloader the first time and whenever a bad flash bricks the USB stack.
4. **USB-C receptacle for programming**: D- to GPIO19, D+ to GPIO20, 5.1 kΩ from
   each CC pin to GND (so a C-to-C cable supplies power), VBUS may also feed the
   5 V rail through a Schottky diode so the board can run from the laptop alone.
   No USB-serial chip is needed; the S3 flashes over its native USB.
   Alternative: a 6-pin header (3V3, GND, EN, GPIO0, U0TXD GPIO43, U0RXD GPIO44)
   for an external USB-serial adapter. Do the USB-C; it is one connector.
5. **Strapping pins**: leave GPIO0, GPIO3, GPIO45, GPIO46 unconnected or on
   buttons only. GPIO17/18 (UART to the Uno) are safe.
6. **Antenna keep-out**: the module's PCB antenna end must hang over the board
   edge or sit over a copper-free zone per the Espressif datasheet (no copper
   under or within ~15 mm around it on any layer). Mounting the module in the
   middle of a ground pour kills the Wi-Fi range.
7. **Footprint**: ESP32-S3-MINI-1 castellated, 65 pads, 1.27 mm pitch, with a
   large thermal/ground pad underneath. KiCad has it in `RF_Module` as
   `ESP32-S3-MINI-1`. Hand-soldering is doable with flux and a fine tip; the
   center pad needs a hot plate or hot air.
8. Keep the test points from section 4.5 and add one on 3V3 and EN.
9. Order the N8 variant (8 MB flash, no PSRAM); N4R2 or others need a different
   partition table.

## 2. DemoFinal's auto-stop compares grams to kilograms

`totalGrams >= (targetkg + currentkg) - 50` mixes units: `targetkg` is 3.0,
so the right side is about -47 and the condition is always true. `stopSystem()`
is then called every loop while idle, and a fill started with `R` is stopped on
the next pass. Demo_v3 holds the target in grams and only checks the stop
condition while a fill is active. Worth confirming with whoever ran the beta
demo that this matches what they saw.

## 3. Totals never reset between fills

DemoFinal accumulates `totalMilliLitres` across fills and offsets by
`currentkg`. A slip needs per-fill numbers, so Demo_v3 zeroes the totals and
the pulse counter at every start. If the LCD was expected to show a running
grand total, that is now gone (it shows the current fill).

## 4. ToF read blocks the loop for up to 500 ms when the sensor misbehaves

`readRangeContinuousMillimeters()` spins until data is ready or the 500 ms
timeout. If the sensor is unplugged or glitches, every loop pass stalls, and a
STOP from the phone waits with it. Demo_v3 polls the data-ready bit and only
reads when a sample exists. Also, DemoFinal halts forever (`while(true)`) if the
sensor fails to init, which would leave the pump uncontrollable from the
dashboard; v3 sets flag bit4 and carries on.

## 5. Density units on the wire

Section 5.1 says `dens` is in lb/gal, but the Uno computes grams per mL
(the 1.02 placeholder is g/mL). Converting on an 8-bit AVR for a display unit
is backwards. Implemented: the wire carries **g/mL**; the ESP32 converts to
lb/gal for display and always prints both on the slip. Section 5.1 of the plan
should be updated to match (or `docs/serial_protocol.md` is now the source).

## 6. Captive portal behaviour on iPhones

iOS opens the portal in its restricted "sign-in" window. In that window there
is no address bar, no add-to-home-screen, and tapping Done can drop the Wi-Fi
if iOS thinks there is no internet. For the demo, tell people: join the
network, tap Cancel, choose "Use Without Internet", then open Safari at
`192.168.4.1`. Android shows a "sign in to network" notification instead and
behaves similarly. This is normal for any offline hotspot; it just needs to be
in the demo script.

## 7. Flow meter ISR vs. Serial on D0/D1

Agree with option 2 (hardware serial). Note that `Serial.print` on the Uno can
also block when its 64-byte TX buffer fills; a telemetry line is about 40 bytes
at 9600 baud (about 42 ms to drain), which fits inside the 500 ms period only
because the debug spam is gone. Do not add prints back into `loop()`.

## 8. Minor

- The plan's `C<a>,<b>` calibration command is accepted by Demo_v3 and stored
  in RAM only; it is lost on Uno reset. The ESP32 keeps the values in
  `settings.json` but does not re-send them on `$E,BOOT` yet. Easy to add once
  the calibration function exists.
- `/live` reloads once per second per phone with the synchronous `WebServer`
  library. Two phones is fine; ten would be sluggish. ESPAsyncWebServer is the
  upgrade if it ever matters.
- The plan asks for a 1.5 MB LittleFS region. `default_8MB.csv` gives exactly
  that. At ~60 bytes per row that is far beyond the 5,000-row cap.
- Open question 5 (Python helper scripts): nothing in this build needs Python.
  The calibration fit can be done in a spreadsheet and typed into Settings.
