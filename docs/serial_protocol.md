# Uno <-> ESP32 serial protocol

9600 baud, 8N1, ASCII, newline (`\n`) terminated. Human-readable on purpose: the
Arduino serial monitor doubles as the debugger. The ESP32 ignores any line that
does not start with `$`. The Uno ignores any command byte it does not know.

Implemented in `esp32-dashboard/uno/Demo_v3.ino` (reference Uno sketch) and `esp32-dashboard/src/uno_link.cpp` (ESP32).

## Uno -> ESP32 telemetry, every 500 ms

```
$T,<phase>,<dens>,<ml>,<g>,<target_g>,<dist_mm>,<flags>
```

| field      | meaning |
|------------|---------|
| `phase`    | `IDLE` / `PRIMING` / `OVERLAP` / `RUNNING` |
| `dens`     | density in **g/mL** (3 decimals), or `-1` when not valid (uncalibrated, or bypass still open). The ESP32 converts to lb/gal for display (x 8.3454). |
| `ml`       | millilitres delivered in the current/last fill (uint) |
| `g`        | grams delivered in the current/last fill (uint) |
| `target_g` | target in grams (uint) |
| `dist_mm`  | raw ToF reading (uint) |
| `flags`    | bitfield: bit0 pump on, bit1 bypass open, bit2 mainline open, bit3 ToF timeout, bit4 sensor init error |

Example: `$T,RUNNING,0.815,1830,1491,3000,142,5`

Totals reset at the start of each fill (differs from DemoFinal, which accumulated).

## Uno -> ESP32 events (once, when they happen)

```
$E,BOOT
$E,START,<target_g>
$E,STOP,<ml>,<g>,<dens>,<reason>     reason: TARGET | MANUAL | ERROR
$E,ERR,<text>                        text: BUSY | BAD_TARGET | BAD_CAL
```

`$E,STOP` becomes a row in `sessions.csv` and a fuel slip. `$E,STOP` is only sent
if something was actually running (a stop while already idle is silent).

## ESP32 -> Uno commands

```
R<kg>\n      set target (0.1 to 50 kg) and start. Rejected with $E,ERR,BUSY unless IDLE.
S\n          stop (reason MANUAL)
P\n B\n M\n  manual toggles for pump / bypass / mainline. Cancel the auto sequence.
C<a>,<b>\n   density calibration coefficients: corrected = a*raw + b (RAM only, phase 2)
```

The Uno also accepts a bare command with no newline (serial monitor set to
"No line ending"): a line is considered complete after 100 ms of idle.

## Wire

Uno D1 (TX, 5 V) -> 1k/2k divider -> ESP32 GPIO18 (RX).
ESP32 GPIO17 (TX, 3.3 V) -> Uno D0 (RX). See `docs/plan_review.md` for why the
series resistor on this line needs a second look before the PCB is ordered.
