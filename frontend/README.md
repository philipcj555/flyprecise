# FlyPrecise dashboard, front end

Static HTML + CSS for every screen of the fuelling dashboard. No JavaScript
anywhere (plan constraint 1). Layout follows the team sketch: three tabs,
NOW / PAST FLIGHTS / ANALYTICS, and on NOW the flight line, fuel requested,
live density, gallons loaded and the FlyPrecise difference.

Open `index.html` in a browser to see it, or serve the folder:

```
cd frontend && python3 -m http.server 8000     # then http://localhost:8000/
```

The numbers in these files are sample data (the sketch's 245,850 lbs flight).
The backend replaces them with live values when it renders each page.

## Files

| file             | screen |
|------------------|--------|
| `index.html`     | NOW. Tab bar, the live panel in an `<iframe>`, the "Load a flight" form, START and STOP |
| `live.html`      | The live panel. Reloads itself every second with `<meta http-equiv="refresh" content="1">`. This is the only thing that refreshes, so the form below it never flickers |
| `past.html`      | PAST FLIGHTS. One row per fuelling, newest first, each linking to its slip |
| `analytics.html` | ANALYTICS. Four tiles, a per-flight difference bar chart (CSS widths), a density table |
| `slip.html`      | Printable fuel slip. White ticket, signature lines, `@media print` rules for letter and 80 mm receipt paper |
| `settings.html`  | Clock, units, planning density, calibration coefficients, hotspot password |
| `advanced.html`  | Manual pump / valve toggles behind a warning. Not in the tab bar |
| `style.css`      | All styling. Colors are CSS variables at the top. System fonts only (no internet at the demo) |
| `manifest.json`, `icon.svg` | Add-to-home-screen name and icon |

## What the backend fills in

Everything else in the files is fixed markup. Links between pages use `.html`
names here so the folder previews on its own; the backend serves them at `/`,
`/live`, `/past`, `/analytics`, `/slip?id=N`, `/settings`, `/advanced`.

**live.html**

| element | value |
|---------|-------|
| `.banner.err` / `.banner.warn` / `.banner.ok` | shown only when true: no link to the Uno, sensor error or timeout, clock not set, "record saved" with a link to its slip |
| `.flight b`, `.flight .route`, `.flight .acft` | flight number, origin → destination, aircraft type of the flight being fuelled (or the last one) |
| `.pill` | text STANDBY / PRIMING / OVERLAP / FUELLING / NO LINK with class `p-idle` / `p-prime` / `p-run` / `p-nolink` |
| `.dot` | add class `on` when pump / bypass / mainline is energised |
| `.ro .num` x4 | fuel requested, live density ("--" when not valid), volume loaded, FlyPrecise difference ("--" when density not valid). Units in the `<small>` follow the Settings choice |
| `.target` text and `.bar div` width | mass loaded so far and percent of requested |

**index.html**: the flash message `<p class="flash ok|err">` after a form post
(started, stopped, saved, bad fuel amount, busy, no link). The fuel input's
unit label and `max` follow the Settings units (50 kg or 110 lbs, the rig limit).

**past.html**: one `<tr>` per flight: record number (link to `slip?id=N`),
flight, route, aircraft, time (or *not set*), requested, loaded, density,
difference, how it ended (TARGET / MANUAL / ERROR).

**analytics.html**: the four tile values; one `.cr` row per flight in the
chart with the bar width as a percent of the largest |difference| and class
`neg` (green) when negative; one table row per flight.

**slip.html**: record number, date/time, flight, route, aircraft, then the
measurements in both unit systems, the difference, and how it ended.

**settings.html**: current values into each input; radio `checked` per the
saved units.

**advanced.html**: `.state` text ON/OFF and class `on` per output; the last
raw event line from the Uno in the `<code>`.

## Forms (what the backend receives)

| form | method / action | fields |
|------|-----------------|--------|
| Load a flight | `POST /start` | `flight`, `acft`, `from`, `to`, `fuel` (in the Settings mass unit) |
| STOP | `POST /stop` | none |
| Settings | `POST /settings` | `dt` (datetime-local), `um` (kg/lb), `uv` (L/gal), `nd` (planning density lbs/gal), `ca`, `cb`, `ap` |
| Advanced | `POST /advanced` | `cmd` = P / B / M |

Each post should answer with `303 See Other` back to `/` (or `/advanced`) so a
browser refresh never re-submits.

## FlyPrecise difference

`fuel requested x (1 - measured density / planning density)`, in lbs.
Planning density defaults to 6.7 lbs/gallon. With the sketch's numbers
(245,850 lbs requested, 6.91 lbs/gal measured) this gives -7,712 lbs. Negative
means the aircraft carries less fuel than a density-assumed load would have
put on board for the same requested mass.

## Reference backend

`../esp32-dashboard/` is a working ESP32 implementation that renders exactly
these pages (the HTML is built in `src/web.cpp`). If the backend is built
elsewhere, treat this folder as the source of truth for markup and keep the
class names, so `style.css` keeps working unchanged.
