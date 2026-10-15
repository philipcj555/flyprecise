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
