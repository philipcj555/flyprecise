// All HTML is rendered server-side. No JavaScript is served, anywhere.
// Layout follows the team's whiteboard sketch: three tabs (NOW, PAST FLIGHTS,
// ANALYTICS); NOW shows the flight line, fuel requested, live density,
// gallons loaded and the FlyPrecise difference.
#include "web.h"
#include "config.h"
#include "uno_link.h"
#include "store.h"
#include <WebServer.h>
#include <WiFi.h>
#include <LittleFS.h>

// Static assets embedded at build time (platformio.ini: board_build.embed_txtfiles).
extern const uint8_t style_css_start[]     asm("_binary_data_style_css_start");
extern const uint8_t style_css_end[]       asm("_binary_data_style_css_end");
extern const uint8_t manifest_json_start[] asm("_binary_data_manifest_json_start");
extern const uint8_t manifest_json_end[]   asm("_binary_data_manifest_json_end");
extern const uint8_t icon_svg_start[]      asm("_binary_data_icon_svg_start");
extern const uint8_t icon_svg_end[]        asm("_binary_data_icon_svg_end");

namespace {
  WebServer server(80);
  uint32_t savedId = 0;
  uint32_t savedAtMs = 0;
  FlightInfo curFlight;

  // ---------- formatting ----------
  // 35578.4 -> "35,578" (dp=0) or "6.91" (dp=2). Thousands separators like the sketch.
  String fmtNum(double v, int dp) {
    bool neg = v < 0;
    if (neg) v = -v;
    char raw[32];
    snprintf(raw, sizeof(raw), "%.*f", dp, v);
    char* dot = strchr(raw, '.');
    size_t intLen = dot ? (size_t)(dot - raw) : strlen(raw);
    String out;
    if (neg) out += '-';
    for (size_t i = 0; i < intLen; i++) {
      out += raw[i];
      size_t rem = intLen - i - 1;
      if (rem && rem % 3 == 0) out += ',';
    }
    if (dot) out += dot;
    return out;
  }
  String esc(const char* s) {
    String o;
    for (; *s; s++) {
      switch (*s) {
        case '<': o += F("&lt;"); break;
        case '>': o += F("&gt;"); break;
        case '&': o += F("&amp;"); break;
        case '"': o += F("&quot;"); break;
        default: o += *s;
      }
    }
    return o;
  }
  // Uppercase, keep letters/digits/dash, cap length. Keeps the CSV clean.
  void sanitize(const String& in, char* out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < in.length() && n < cap - 1; i++) {
      char c = in[i];
      if (isalnum((unsigned char)c) || c == '-') out[n++] = toupper((unsigned char)c);
    }
    out[n] = '\0';
  }

  // ---------- units ----------
  const Settings& S() { return Store::settings(); }
  const char* massUnit() { return S().massLb() ? "lbs" : "kg"; }
  const char* volUnit()  { return S().volGal() ? "gallons" : "litres"; }
  const char* densUnit() { return S().volGal() ? "lbs/gallon" : "g/mL"; }
  String fmtMass(uint32_t g)  { return S().massLb() ? fmtNum(g * G_TO_LB, g * G_TO_LB < 100 ? 2 : 0) : fmtNum(g / 1000.0, 3); }
  String fmtMassLb(float lb)  { return S().massLb() ? fmtNum(lb, fabs(lb) < 100 ? 2 : 0) : fmtNum(lb * 0.45359237f, 3); }
  String fmtVol(uint32_t ml)  { return S().volGal() ? fmtNum(ml * ML_TO_GAL, 3) : fmtNum(ml / 1000.0, 2); }
  String fmtDens(float gml)   { if (gml < 0) return "--"; return S().volGal() ? fmtNum(gml * GML_TO_LBGAL, 2) : fmtNum(gml, 3); }
  String fmtDiff(uint32_t targetG, float densGml) {
    float d;
    if (!Store::differenceLb(targetG, densGml, d)) return "--";
    return fmtMassLb(d);
  }
  float inputToKg(float v) { return S().massLb() ? v * 0.45359237f : v; }

  // ---------- page scaffolding ----------
  void sendNoCache() { server.sendHeader("Cache-Control", "no-store"); }
  void redirect(const String& to) {
    server.sendHeader("Location", to);
    sendNoCache();
    server.send(303, "text/plain", "");
  }
  String head(const char* title, bool live = false) {
    String h;
    h.reserve(700);
    h += F("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    if (live) h += F("<meta http-equiv=\"refresh\" content=\"1\">");
    h += F("<title>FlyPrecise");
    if (title[0]) { h += F(" | "); h += title; }
    h += F("</title><link rel=\"stylesheet\" href=\"/style.css\">"
           "<link rel=\"manifest\" href=\"/manifest.json\">"
           "<meta name=\"theme-color\" content=\"#0b0d10\">"
           "<link rel=\"icon\" href=\"/icon.svg\" type=\"image/svg+xml\">"
           "<link rel=\"apple-touch-icon\" href=\"/icon.svg\">"
           "<meta name=\"apple-mobile-web-app-capable\" content=\"yes\">"
           "</head><body");
    if (live) h += F(" class=\"live\"");
    h += F(">");
    return h;
  }
  // Tab bar from the sketch: NOW | PAST FLIGHTS | ANALYTICS
  String tabs(const char* active) {
    String n;
    n.reserve(360);
    n += F("<header><a class=\"brand\" href=\"/\">Fly<span>Precise</span></a><nav class=\"tabs\">");
    const char* items[][2] = { {"/", "NOW"}, {"/past", "PAST FLIGHTS"}, {"/analytics", "ANALYTICS"} };
    for (auto& it : items) {
      n += F("<a href=\""); n += it[0]; n += F("\"");
      if (!strcmp(it[1], active)) n += F(" class=\"on\"");
      n += F(">"); n += it[1]; n += F("</a>");
    }
    n += F("</nav><a class=\"gear\" href=\"/settings\" title=\"Settings\">&#9881;</a></header>");
    return n;
  }
  const char* foot = "</body></html>";

  String flightLine(const FlightInfo& fi) {
    String s;
    s.reserve(160);
    s += F("<div class=\"flight\"><b>"); s += esc(fi.flight); s += F("</b><span class=\"route\">");
    s += esc(fi.origin); s += F(" <i>&rarr;</i> "); s += esc(fi.dest);
    s += F("</span><span class=\"acft\">"); s += esc(fi.aircraft); s += F("</span></div>");
    return s;
  }
  void readout(String& p, const char* label, const String& value, const char* unit, const char* cls = "") {
    p += F("<div class=\"ro "); p += cls; p += F("\"><span class=\"lbl\">"); p += label;
    p += F("</span><span class=\"num\">"); p += value; p += F(" <small>"); p += unit; p += F("</small></span></div>");
  }

  // ---------- / (NOW) ----------
  void handleRoot() {
    String m = server.arg("m");
    String p = head("", false);
    p.reserve(3200);
    p += tabs("NOW");
    p += F("<main>");
    if (m == "started")      p += F("<p class=\"flash ok\">Fuelling started.</p>");
    else if (m == "stopped") p += F("<p class=\"flash ok\">Stop sent.</p>");
    else if (m == "badfuel") p += F("<p class=\"flash err\">Fuel requested must be between 0.1 kg and 50 kg (0.2 to 110 lbs) on this rig.</p>");
    else if (m == "noflight")p += F("<p class=\"flash err\">Enter a flight number first.</p>");
    else if (m == "busy")    p += F("<p class=\"flash err\">Fuelling already in progress. Press STOP first.</p>");
    else if (m == "nolink")  p += F("<p class=\"flash err\">No link to the Uno. Command not sent.</p>");
    else if (m == "saved")   p += F("<p class=\"flash ok\">Settings saved.</p>");
    p += F("<iframe class=\"live\" src=\"/live\" title=\"Live readings\"></iframe>");

    p += F("<form class=\"load\" method=\"post\" action=\"/start\"><h2>Load a flight</h2><div class=\"grid\">"
           "<label>Flight<input name=\"flight\" type=\"text\" maxlength=\"10\" placeholder=\"XX1234\" autocomplete=\"off\" required></label>"
           "<label>Aircraft<input name=\"acft\" type=\"text\" maxlength=\"14\" placeholder=\"B787-900\" autocomplete=\"off\"></label>"
           "<label>From<input name=\"from\" type=\"text\" maxlength=\"4\" placeholder=\"LAX\" autocomplete=\"off\"></label>"
           "<label>To<input name=\"to\" type=\"text\" maxlength=\"4\" placeholder=\"SIN\" autocomplete=\"off\"></label>"
           "</div><label class=\"fuel\">Fuel requested (");
    p += massUnit();
    p += F(")<input name=\"fuel\" type=\"number\" inputmode=\"decimal\" min=\"0.1\" max=\"");
    p += S().massLb() ? "110" : "50";
    p += F("\" step=\"0.1\" placeholder=\"0.0\" required></label>"
           "<button class=\"btn start\" type=\"submit\">START FUELLING</button></form>"
           "<form method=\"post\" action=\"/stop\"><button class=\"btn stop\" type=\"submit\">STOP</button></form>"
           "<p class=\"hint\">Readings refresh every second. Rig limit 50 kg per fill. <a href=\"/advanced\">Advanced</a></p></main>");
    p += foot;
    sendNoCache();
    server.send(200, "text/html", p);
  }

  // ---------- /live ----------
  const char* pillClass(const Telemetry& t) {
    if (!t.linkOk()) return "p-nolink";
    switch (t.phase) {
      case Phase::IDLE:    return "p-idle";
      case Phase::PRIMING:
      case Phase::OVERLAP: return "p-prime";
      case Phase::RUNNING: return "p-run";
      default:             return "p-nolink";
    }
  }
  const char* pillText(const Telemetry& t) {
    if (!t.linkOk()) return "NO LINK";
    switch (t.phase) {
      case Phase::IDLE:    return "STANDBY";
      case Phase::PRIMING: return "PRIMING";
      case Phase::OVERLAP: return "OVERLAP";
      case Phase::RUNNING: return "FUELLING";
      default:             return "NO LINK";
    }
  }
  void dot(String& p, const char* label, bool on) {
    p += F("<span class=\"dot"); if (on) p += F(" on"); p += F("\"><i></i>"); p += label; p += F("</span>");
  }
  void handleLive() {
    const Telemetry& t = UnoLink::telemetry();
    String p = head("Live", true);
    p.reserve(2600);
    if (!t.linkOk())
      p += F("<div class=\"banner err\">No data from the Uno. Check the serial link.</div>");
    if (t.sensorError())
      p += F("<div class=\"banner err\">Density sensor failed to initialise.</div>");
    else if (t.tofTimeout())
      p += F("<div class=\"banner warn\">Density sensor timeout. Reading unreliable.</div>");
    if (!Store::clockSet())
      p += F("<div class=\"banner warn\">Clock not set. <a href=\"/settings\" target=\"_top\">Set it</a> so records get a date.</div>");
    if (savedId && millis() - savedAtMs < 60000) {
      p += F("<div class=\"banner ok\">Flight record "); p += savedId;
      p += F(" saved. <a href=\"/slip?id=");  p += savedId; p += F("\" target=\"_top\">Open fuel slip</a></div>");
    }
    if (UnoLink::lastErrorMs() && millis() - UnoLink::lastErrorMs() < 30000) {
      p += F("<div class=\"banner err\">Uno error: "); p += esc(UnoLink::lastError()); p += F("</div>");
    }

    // Flight line: the flight being fuelled, else the last one on record.
    if (curFlight.set()) p += flightLine(curFlight);
    else {
      SessionRow last;
      if (Store::lastSession(last) && last.flight.set()) p += flightLine(last.flight);
      else p += F("<div class=\"flight none\">No flight loaded</div>");
    }

    p += F("<div class=\"status\"><span class=\"pill "); p += pillClass(t); p += F("\">"); p += pillText(t);
    p += F("</span><span class=\"dots\">");
    dot(p, "Pump", t.pump()); dot(p, "Bypass", t.bypass()); dot(p, "Main", t.mainline());
    p += F("</span></div><div class=\"readouts\">");

    readout(p, "Fuel requested", t.targetG ? fmtMass(t.targetG) : String("--"), massUnit());
    readout(p, "Live density", fmtDens(t.densGml), densUnit());
    readout(p, S().volGal() ? "Gallons loaded" : "Litres loaded", fmtVol(t.ml), volUnit(), "big");
    readout(p, "FlyPrecise difference", fmtDiff(t.targetG, t.densGml), massUnit(), "diff");
    p += F("</div>");

    if (t.targetG) {
      uint32_t pct = t.g >= t.targetG ? 100 : (t.g * 100UL) / t.targetG;
      p += F("<div class=\"target\"><span>"); p += fmtMass(t.g); p += ' '; p += massUnit(); p += F(" loaded</span><span>");
      p += pct; p += F("%</span></div><div class=\"bar\"><div style=\"width:"); p += pct; p += F("%\"></div></div>");
    }
    p += foot;
    sendNoCache();
    server.send(200, "text/html", p);
  }

  // ---------- /start /stop ----------
  void handleStart() {
    const Telemetry& t = UnoLink::telemetry();
    FlightInfo fi;
    sanitize(server.arg("flight"), fi.flight, sizeof(fi.flight));
    sanitize(server.arg("from"), fi.origin, sizeof(fi.origin));
    sanitize(server.arg("to"), fi.dest, sizeof(fi.dest));
    sanitize(server.arg("acft"), fi.aircraft, sizeof(fi.aircraft));
    if (!fi.set()) return redirect("/?m=noflight");
    float kg = inputToKg(server.arg("fuel").toFloat());
    if (!(kg >= TARGET_MIN_KG && kg <= TARGET_MAX_KG)) return redirect("/?m=badfuel");
#if !FAKE_UNO
    if (!t.linkOk()) return redirect("/?m=nolink");
#endif
    if (!t.idle()) return redirect("/?m=busy");
    curFlight = fi;
    UnoLink::sendStart(kg);
    redirect("/?m=started");
  }
  void handleStop() {
    UnoLink::sendStop();
    redirect("/?m=stopped");
  }

  // ---------- /past ----------
  void handlePast() {
    static SessionRow rows[HISTORY_ROWS];
    size_t n = Store::recentSessions(rows, HISTORY_ROWS);
    String p = head("Past flights");
    p.reserve(1400 + n * 260);
    p += tabs("PAST FLIGHTS");
    p += F("<main><div class=\"row\"><h1>Past flights</h1><a class=\"btn small\" href=\"/export.csv\">Export CSV</a></div>");
    if (!n) {
      p += F("<p class=\"hint\">No flights yet. Completed fuellings appear here.</p>");
    } else {
      p += F("<div class=\"tablewrap\"><table><thead><tr><th>#</th><th>Flight</th><th>Route</th><th>Aircraft</th><th>Time</th>"
             "<th>Requested</th><th>Loaded</th><th>Density</th><th>Difference</th><th>End</th></tr></thead><tbody>");
      for (size_t i = 0; i < n; i++) {
        const SessionRow& r = rows[i];
        p += F("<tr><td><a href=\"/slip?id="); p += r.id; p += F("\">"); p += r.id; p += F("</a></td><td>");
        p += esc(r.flight.flight); p += F("</td><td>"); p += esc(r.flight.origin); p += F(" &rarr; "); p += esc(r.flight.dest);
        p += F("</td><td>"); p += esc(r.flight.aircraft); p += F("</td><td>");
        String ts = Store::formatTime(r.epoch);
        p += ts.length() ? ts : String("<i>not set</i>");
        p += F("</td><td>"); p += fmtMass(r.targetG); p += ' '; p += massUnit();
        p += F("</td><td>"); p += fmtVol(r.ml); p += ' '; p += S().volGal() ? "gal" : "L";
        p += F("</td><td>"); p += fmtDens(r.densGml);
        p += F("</td><td>"); p += fmtDiff(r.targetG, r.densGml); p += ' '; p += massUnit();
        p += F("</td><td>"); p += r.reason; p += F("</td></tr>");
      }
      p += F("</tbody></table></div>");
      if (n == HISTORY_ROWS) p += F("<p class=\"hint\">Showing the latest 50. Export the CSV for everything.</p>");
    }
    p += F("</main>");
    p += foot;
    sendNoCache();
    server.send(200, "text/html", p);
  }

  // ---------- /analytics ----------
  void tile(String& p, const char* label, const String& value, const char* unit) {
    p += F("<div class=\"tile\"><div class=\"tv\">"); p += value; p += F(" <small>"); p += unit;
    p += F("</small></div><div class=\"tl\">"); p += label; p += F("</div></div>");
  }
  void handleAnalytics() {
    Stats st;
    Store::computeStats(st);
    static SessionRow rows[ANALYTICS_BARS];
    size_t n = Store::recentSessions(rows, ANALYTICS_BARS);
    String p = head("Analytics");
    p.reserve(2400 + n * 220);
    p += tabs("ANALYTICS");
    p += F("<main><h1>Analytics</h1>");
    if (!st.flights) {
      p += F("<p class=\"hint\">Nothing to analyse yet. Fuel a flight first.</p></main>");
      p += foot; sendNoCache(); server.send(200, "text/html", p); return;
    }
    p += F("<div class=\"tiles\">");
    tile(p, "Flights fuelled", fmtNum(st.flights, 0), "");
    tile(p, "Fuel loaded", fmtMass((uint32_t)st.totalG), massUnit());
    tile(p, "FlyPrecise difference, total", st.flightsWithDensity ? fmtMassLb((float)st.totalDiffLb) : String("--"), massUnit());
    tile(p, "Average density", st.flightsWithDensity ? fmtDens((float)(st.sumDensGml / st.flightsWithDensity)) : String("--"), densUnit());
    p += F("</div><p class=\"hint\">Difference = fuel requested &times; (1 &minus; measured density / planning density of ");
    p += fmtNum(S().nominalLbGal, 2);
    p += F(" lbs/gallon). Negative means less fuel carried than a density-assumed load for the same requested mass.</p>");

    // Per-flight difference bars, last 12 flights, newest at the bottom. Pure CSS.
    float maxAbs = 0;
    for (size_t i = 0; i < n; i++) { float d; if (Store::differenceLb(rows[i].targetG, rows[i].densGml, d) && fabs(d) > maxAbs) maxAbs = fabs(d); }
    p += F("<h2>Difference per flight</h2><div class=\"chart\">");
    for (size_t k = 0; k < n; k++) {
      const SessionRow& r = rows[n - 1 - k];   // oldest first
      float d; bool ok = Store::differenceLb(r.targetG, r.densGml, d);
      int pct = (ok && maxAbs > 0) ? (int)(fabs(d) / maxAbs * 100) : 0;
      p += F("<div class=\"cr\"><span class=\"cl\">"); p += esc(r.flight.flight[0] ? r.flight.flight : "#");
      if (!r.flight.flight[0]) p += r.id;
      p += F("</span><span class=\"cb\"><i class=\""); p += (ok && d < 0) ? "neg" : "pos";
      p += F("\" style=\"width:"); p += pct; p += F("%\"></i></span><span class=\"cv\">");
      p += ok ? fmtMassLb(d) : String("--"); p += F("</span></div>");
    }
    p += F("</div><h2>Density per flight</h2><div class=\"tablewrap\"><table><thead><tr><th>Flight</th><th>Density</th><th>Loaded</th><th>Requested</th></tr></thead><tbody>");
    for (size_t i = 0; i < n; i++) {
      const SessionRow& r = rows[i];
      p += F("<tr><td>"); p += esc(r.flight.flight); p += F("</td><td>"); p += fmtDens(r.densGml); p += F("</td><td>");
      p += fmtVol(r.ml); p += F("</td><td>"); p += fmtMass(r.targetG); p += F("</td></tr>");
    }
    p += F("</tbody></table></div></main>");
    p += foot;
    sendNoCache();
    server.send(200, "text/html", p);
  }

  // ---------- /slip ----------
  void slipLine(String& p, const char* k, const String& v) {
    p += F("<div class=\"ln\"><span>"); p += k; p += F("</span><b>"); p += v; p += F("</b></div>");
  }
  void handleSlip() {
    uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
    SessionRow r;
    if (!id || !Store::findSession(id, r)) { server.send(404, "text/plain", "No such flight record"); return; }
    String p = head("Fuel slip");
    p.reserve(3000);
    p += tabs("");
    p += F("<main class=\"slip\"><div class=\"ticket\"><div class=\"tk-head\"><div class=\"tk-brand\">FlyPrecise</div><div class=\"tk-sub\">Digital fuel slip</div></div>");
    slipLine(p, "Record", String("#") + r.id);
    String ts = Store::formatTime(r.epoch);
    slipLine(p, "Date / time", ts.length() ? ts : String("(time not set)"));
    slipLine(p, "Flight", esc(r.flight.flight));
    slipLine(p, "Route", esc(r.flight.origin) + " &rarr; " + esc(r.flight.dest));
    slipLine(p, "Aircraft", esc(r.flight.aircraft));
    p += F("<hr>");
    slipLine(p, "Fuel requested", fmtNum(r.targetG * G_TO_LB, 2) + " lbs &nbsp;/&nbsp; " + fmtNum(r.targetG / 1000.0, 3) + " kg");
    slipLine(p, "Volume loaded", fmtNum(r.ml * ML_TO_GAL, 3) + " gal &nbsp;/&nbsp; " + fmtNum(r.ml / 1000.0, 2) + " L");
    slipLine(p, "Density", r.densGml < 0 ? String("--") :
             fmtNum(r.densGml * GML_TO_LBGAL, 2) + " lbs/gal &nbsp;/&nbsp; " + fmtNum(r.densGml, 3) + " g/mL");
    slipLine(p, "Mass loaded", fmtNum(r.g * G_TO_LB, 2) + " lbs &nbsp;/&nbsp; " + fmtNum(r.g / 1000.0, 3) + " kg");
    { float d; slipLine(p, "FlyPrecise difference", Store::differenceLb(r.targetG, r.densGml, d) ? fmtNum(d, 2) + " lbs" : String("--")); }
    slipLine(p, "Stopped by", String(r.reason));
    p += F("<hr><div class=\"fill\"><span>Aircraft tail #</span><em></em></div>"
           "<div class=\"fill\"><span>Fueller</span><em></em></div>"
           "<div class=\"fill\"><span>Signature</span><em></em></div>"
           "<div class=\"tk-foot\">Measured by flow meter and in-line densimeter. Not a legal-for-trade instrument.</div>"
           "</div><p class=\"hint noprint\">Print: phone share menu &rarr; Print, or Ctrl+P. <a href=\"/past\">Back to past flights</a></p></main>");
    p += foot;
    sendNoCache();
    server.send(200, "text/html", p);
  }

  // ---------- /export.csv ----------
  void handleExport() {
    File f = LittleFS.open(SESSIONS_PATH, "r");
    if (!f) { server.send(404, "text/plain", "no sessions file"); return; }
    server.sendHeader("Content-Disposition", "attachment; filename=\"flyprecise_flights.csv\"");
    sendNoCache();
    server.streamFile(f, "text/csv");
    f.close();
  }

  // ---------- /settings ----------
  uint32_t civilToEpoch(int y, unsigned m, unsigned d, unsigned hh, unsigned mm) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + (long)doe - 719468L;
    return (uint32_t)(days * 86400L + hh * 3600L + mm * 60L);
  }
  void radio(String& p, const char* name, const char* val, const char* label, bool on) {
    p += F("<label class=\"opt\"><input type=\"radio\" name=\""); p += name; p += F("\" value=\""); p += val; p += F("\"");
    if (on) p += F(" checked");
    p += F("> "); p += label; p += F("</label>");
  }
  void handleSettingsGet() {
    const Settings& s = S();
    String p = head("Settings");
    p.reserve(3000);
    p += tabs("");
    p += F("<main><h1>Settings</h1><form method=\"post\" action=\"/settings\" class=\"settings\">");
    p += F("<fieldset><legend>Clock</legend><p class=\"hint\">The ESP32 has no battery clock. Set this once per power-up so records get a date.</p>"
           "<label for=\"dt\">Current date &amp; time</label><input id=\"dt\" name=\"dt\" type=\"datetime-local\" value=\"");
    p += Store::formatTimeForInput(Store::nowEpoch());
    p += F("\"></fieldset>");
    p += F("<fieldset><legend>Units</legend><div class=\"opts\">");
    radio(p, "um", "lb", "lbs", s.massLb());
    radio(p, "um", "kg", "kg", !s.massLb());
    p += F("</div><div class=\"opts\">");
    radio(p, "uv", "gal", "gallons (density in lbs/gallon)", s.volGal());
    radio(p, "uv", "L", "litres (density in g/mL)", !s.volGal());
    p += F("</div><p class=\"hint\">Slips always show both.</p></fieldset>");
    p += F("<fieldset><legend>Planning density</legend><p class=\"hint\">The density a conventional load assumes. The FlyPrecise difference is measured against it. Jet A is usually planned at 6.7 lbs/gallon.</p>"
           "<label for=\"nd\">lbs per gallon</label><input id=\"nd\" name=\"nd\" type=\"number\" step=\"0.01\" min=\"5\" max=\"9\" value=\"");
    p += String(s.nominalLbGal, 2);
    p += F("\"></fieldset>");
    p += F("<fieldset><legend>Density calibration</legend><p class=\"hint\">corrected = a &times; raw + b. Sent to the Uno as <code>C&lt;a&gt;,&lt;b&gt;</code>. Leave at 1 / 0 until the Oct 19 calibration is done.</p>"
           "<label for=\"ca\">a</label><input id=\"ca\" name=\"ca\" type=\"number\" step=\"any\" value=\"");
    p += String(s.calA, 5);
    p += F("\"><label for=\"cb\">b</label><input id=\"cb\" name=\"cb\" type=\"number\" step=\"any\" value=\"");
    p += String(s.calB, 5);
    p += F("\"></fieldset>");
    p += F("<fieldset><legend>Wi-Fi</legend><label for=\"ap\">Hotspot password (8+ chars, applies after reboot; blank = keep)</label>"
           "<input id=\"ap\" name=\"ap\" type=\"text\" minlength=\"8\" maxlength=\"32\" autocomplete=\"off\"></fieldset>");
    p += F("<button class=\"btn start\" type=\"submit\">Save</button></form>");
    p += F("<p class=\"hint\">Storage: "); p += Store::storageOk() ? "OK" : "FAILED";
    p += F(", flights file "); p += (unsigned)Store::sessionsFileSize();
    p += F(" bytes. <a href=\"/export.csv\">Export</a> &nbsp; <a href=\"/advanced\">Advanced</a> &nbsp; <a href=\"/debug\">Debug</a></p></main>");
    p += foot;
    sendNoCache();
    server.send(200, "text/html", p);
  }
  void handleSettingsPost() {
    Settings& s = Store::settings();
    String dt = server.arg("dt");
    int y, mo, d, hh, mi;
    if (dt.length() >= 16 && sscanf(dt.c_str(), "%d-%d-%dT%d:%d", &y, &mo, &d, &hh, &mi) == 5 && y >= 2024) {
      uint32_t e = civilToEpoch(y, mo, d, hh, mi);
      if (e != Store::nowEpoch()) Store::setClock(e);
    }
    String um = server.arg("um"), uv = server.arg("uv");
    if (um == "kg" || um == "lb") strlcpy(s.unitsMass, um.c_str(), sizeof(s.unitsMass));
    if (uv == "L" || uv == "gal") strlcpy(s.unitsVol, uv.c_str(), sizeof(s.unitsVol));
    float nd = server.arg("nd").toFloat();
    if (nd >= 5.0f && nd <= 9.0f) s.nominalLbGal = nd;
    if (server.hasArg("ca") && server.hasArg("cb")) {
      float a = server.arg("ca").toFloat(), b = server.arg("cb").toFloat();
      if (a != s.calA || b != s.calB) { s.calA = a; s.calB = b; UnoLink::sendCalibration(a, b); }
    }
    String ap = server.arg("ap");
    if (ap.length() >= 8 && ap.length() <= 32) strlcpy(s.apPass, ap.c_str(), sizeof(s.apPass));
    Store::saveSettings();
    redirect("/?m=saved");
  }

  // ---------- /advanced ----------
  void handleAdvancedGet() {
    const Telemetry& t = UnoLink::telemetry();
    String p = head("Advanced");
    p.reserve(1900);
    p += tabs("");
    p += F("<main><h1>Advanced</h1><div class=\"banner err\">Manual valve and pump control. Toggling anything here cancels the automatic sequence on the Uno. "
           "Running the pump with both valves closed dead-heads it.</div><div class=\"adv\">");
    const char* cmds[][2] = { {"P", "Pump"}, {"B", "Bypass valve"}, {"M", "Mainline valve"} };
    bool st[3] = { t.pump(), t.bypass(), t.mainline() };
    for (int i = 0; i < 3; i++) {
      p += F("<form method=\"post\" action=\"/advanced\"><input type=\"hidden\" name=\"cmd\" value=\""); p += cmds[i][0];
      p += F("\"><button class=\"btn adv-btn\" type=\"submit\"><span>"); p += cmds[i][1];
      p += F("</span><b class=\"state "); p += st[i] ? "on\">ON" : "\">OFF"; p += F("</b></button></form>");
    }
    p += F("</div><form method=\"post\" action=\"/stop\"><button class=\"btn stop\" type=\"submit\">STOP ALL</button></form>");
    p += F("<p class=\"hint\">Last Uno event: <code>"); p += esc(UnoLink::lastEvent());
    p += F("</code> &nbsp; <a href=\"/debug\">debug</a> &nbsp; <a href=\"/\">back</a></p></main>");
    p += foot;
    sendNoCache();
    server.send(200, "text/html", p);
  }
  void handleAdvancedPost() {
    String c = server.arg("cmd");
    if (c == "P" || c == "B" || c == "M") UnoLink::sendRaw(c.c_str());
    redirect("/advanced");
  }

  // ---------- /debug (plain text) ----------
  void handleDebug() {
    const Telemetry& t = UnoLink::telemetry();
    String p;
    p.reserve(600);
    p += "FlyPrecise ESP32 debug\n";
    p += "fake_uno: "; p += FAKE_UNO; p += '\n';
    p += "uptime_s: "; p += millis() / 1000; p += '\n';
    p += "free_heap: "; p += ESP.getFreeHeap(); p += '\n';
    p += "ap_clients: "; p += WiFi.softAPgetStationNum(); p += '\n';
    p += "link_ok: "; p += t.linkOk(); p += "  last_rx_ms_ago: "; p += t.everReceived ? millis() - t.lastRxMs : 0; p += '\n';
    p += "phase: "; p += phaseName(t.phase); p += "  flags: "; p += t.flags; p += '\n';
    p += "dens_gml: "; p += String(t.densGml, 3); p += "  ml: "; p += t.ml; p += "  g: "; p += t.g;
    p += "  target_g: "; p += t.targetG; p += "  dist_mm: "; p += t.distMm; p += '\n';
    p += "flight: "; p += curFlight.flight; p += ' '; p += curFlight.origin; p += '-'; p += curFlight.dest; p += ' '; p += curFlight.aircraft; p += '\n';
    p += "last_event: "; p += UnoLink::lastEvent(); p += '\n';
    p += "clock_set: "; p += Store::clockSet(); p += "  epoch: "; p += Store::nowEpoch(); p += '\n';
    p += "sessions_bytes: "; p += (unsigned)Store::sessionsFileSize(); p += "  next_id: "; p += Store::settings().nextId; p += '\n';
    sendNoCache();
    server.send(200, "text/plain", p);
  }

  // ---------- static ----------
  void sendEmbedded(const uint8_t* s, const uint8_t* e, const char* type, bool cache) {
    size_t len = e - s;
    if (len && s[len - 1] == '\0') len--;
    server.sendHeader("Cache-Control", cache ? "max-age=86400" : "no-store");
    server.send_P(200, type, (const char*)s, len);
  }
  void handleCss()      { sendEmbedded(style_css_start, style_css_end, "text/css", true); }
  void handleManifest() { sendEmbedded(manifest_json_start, manifest_json_end, "application/manifest+json", true); }
  void handleIcon()     { sendEmbedded(icon_svg_start, icon_svg_end, "image/svg+xml", true); }
  void handleNotFound() { redirect(String("http://") + AP_IP.toString() + "/"); }
}

void Web::noteSessionSaved(uint32_t id) { savedId = id; savedAtMs = millis(); }
const FlightInfo& Web::currentFlight() { return curFlight; }

void Web::begin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/live", HTTP_GET, handleLive);
  server.on("/start", HTTP_POST, handleStart);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/past", HTTP_GET, handlePast);
  server.on("/history", HTTP_GET, handlePast);
  server.on("/analytics", HTTP_GET, handleAnalytics);
  server.on("/slip", HTTP_GET, handleSlip);
  server.on("/export.csv", HTTP_GET, handleExport);
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/advanced", HTTP_GET, handleAdvancedGet);
  server.on("/advanced", HTTP_POST, handleAdvancedPost);
  server.on("/debug", HTTP_GET, handleDebug);
  server.on("/style.css", HTTP_GET, handleCss);
  server.on("/manifest.json", HTTP_GET, handleManifest);
  server.on("/icon.svg", HTTP_GET, handleIcon);
  const char* probes[] = { "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                           "/connecttest.txt", "/ncsi.txt", "/success.txt", "/canonical.html", "/redirect" };
  for (auto u : probes) server.on(u, handleNotFound);
  server.onNotFound(handleNotFound);
  server.begin();
}

void Web::loop() { server.handleClient(); }
