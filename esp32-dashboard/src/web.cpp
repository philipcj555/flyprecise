// All HTML is rendered server-side. No JavaScript is served, anywhere.
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

  // ---------- unit helpers ----------
  String fmtMass(uint32_t g, int dp = 3) {
    return Store::settings().massLb() ? String(g * G_TO_LB, dp) : String(g / 1000.0f, dp);
  }
  const char* massUnit() { return Store::settings().massLb() ? "lb" : "kg"; }
  String fmtVol(uint32_t ml) {
    return Store::settings().volGal() ? String(ml * ML_TO_GAL, 3) : String(ml / 1000.0f, 2);
  }
  const char* volUnit() { return Store::settings().volGal() ? "gal" : "L"; }
  String fmtDens(float gml) {
    if (gml < 0) return "--";
    return Store::settings().volGal() ? String(gml * GML_TO_LBGAL, 2) : String(gml, 3);
  }
  const char* densUnit() { return Store::settings().volGal() ? "lb/gal" : "g/mL"; }
  float inputToKg(float v) { return Store::settings().massLb() ? v * 0.45359237f : v; }

  // ---------- page scaffolding ----------
  void sendNoCache() {
    server.sendHeader("Cache-Control", "no-store");
  }
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
    h += F("<title>");
    h += title;
    h += F("</title><link rel=\"stylesheet\" href=\"/style.css\">"
           "<link rel=\"manifest\" href=\"/manifest.json\">"
           "<meta name=\"theme-color\" content=\"#0b0d10\">"
           "<link rel=\"icon\" href=\"/icon.svg\" type=\"image/svg+xml\">"
           "<link rel=\"apple-touch-icon\" href=\"/icon.svg\">"
           "<meta name=\"apple-mobile-web-app-capable\" content=\"yes\">"
           "<meta name=\"apple-mobile-web-app-status-bar-style\" content=\"black-translucent\">"
           "</head><body");
    if (live) h += F(" class=\"live\"");
    h += F(">");
    return h;
  }
  String nav(const char* active) {
    String n;
    n.reserve(300);
    n += F("<header><a class=\"brand\" href=\"/\">Fly<span>Precise</span></a><nav>");
    const char* items[][2] = { {"/", "Live"}, {"/history", "History"}, {"/settings", "Settings"} };
    for (auto& it : items) {
      n += F("<a href=\"");
      n += it[0];
      n += F("\"");
      if (!strcmp(it[1], active)) n += F(" class=\"on\"");
      n += F(">");
      n += it[1];
      n += F("</a>");
    }
    n += F("</nav></header>");
    return n;
  }
  const char* foot = "</body></html>";

  // ---------- / ----------
  void handleRoot() {
    const Settings& s = Store::settings();
    String m = server.arg("m");
    String p = head("FlyPrecise");
    p.reserve(2600);
    p += nav("Live");
    p += F("<main>");
    if (m == "started")   p += F("<p class=\"flash ok\">Start sent to the Uno.</p>");
    else if (m == "stopped") p += F("<p class=\"flash ok\">Stop sent to the Uno.</p>");
    else if (m == "badkg")   p += F("<p class=\"flash err\">Target must be between 0.1 and 50 kg.</p>");
    else if (m == "busy")    p += F("<p class=\"flash err\">Fill already in progress. Press STOP first.</p>");
    else if (m == "nolink")  p += F("<p class=\"flash err\">No link to the Uno. Command not sent.</p>");
    else if (m == "saved")   p += F("<p class=\"flash ok\">Settings saved.</p>");
    p += F("<iframe class=\"live\" src=\"/live\" title=\"Live readings\"></iframe>");
    p += F("<form class=\"start\" method=\"post\" action=\"/start\">"
           "<label for=\"kg\">Target mass (");
    p += s.massLb() ? "lb" : "kg";
    p += F(")</label><input id=\"kg\" name=\"kg\" type=\"number\" inputmode=\"decimal\" "
           "min=\"0.1\" max=\"");
    p += s.massLb() ? "110" : "50";
    p += F("\" step=\"0.1\" placeholder=\"0.0\" required>"
           "<button class=\"btn start\" type=\"submit\">START</button></form>"
           "<form method=\"post\" action=\"/stop\"><button class=\"btn stop\" type=\"submit\">STOP</button></form>"
           "<p class=\"hint\">Live panel refreshes every second. "
           "<a href=\"/advanced\">Advanced</a></p></main>");
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
  void dot(String& p, const char* label, bool on) {
    p += F("<span class=\"dot");
    if (on) p += F(" on");
    p += F("\"><i></i>");
    p += label;
    p += F("</span>");
  }
  void handleLive() {
    const Telemetry& t = UnoLink::telemetry();
    String p = head("Live", true);
    p.reserve(2200);
    if (!t.linkOk())
      p += F("<div class=\"banner err\">No data from the Uno. Check the serial link.</div>");
    if (t.sensorError())
      p += F("<div class=\"banner err\">ToF sensor failed to initialise.</div>");
    else if (t.tofTimeout())
      p += F("<div class=\"banner warn\">ToF sensor timeout. Density reading unreliable.</div>");
    if (!Store::clockSet())
      p += F("<div class=\"banner warn\">Clock not set. <a href=\"/settings\" target=\"_top\">Set it</a> so slips get a date.</div>");
    if (savedId && millis() - savedAtMs < 60000) {
      p += F("<div class=\"banner ok\">Session ");
      p += savedId;
      p += F(" saved. <a href=\"/slip?id=");
      p += savedId;
      p += F("\" target=\"_top\">Open fuel slip</a></div>");
    }
    if (UnoLink::lastErrorMs() && millis() - UnoLink::lastErrorMs() < 30000) {
      p += F("<div class=\"banner err\">Uno error: ");
      p += UnoLink::lastError();
      p += F("</div>");
    }

    p += F("<div class=\"status\"><span class=\"pill ");
    p += pillClass(t);
    p += F("\">");
    p += phaseName(t.linkOk() ? t.phase : Phase::UNKNOWN);
    p += F("</span><span class=\"dots\">");
    dot(p, "Pump", t.pump());
    dot(p, "Bypass", t.bypass());
    dot(p, "Main", t.mainline());
    p += F("</span></div>");

    p += F("<div class=\"readouts\">");
    p += F("<div class=\"ro\"><div class=\"val\">"); p += fmtDens(t.densGml); p += F("</div><div class=\"unit\">");
    p += densUnit(); p += F(" density</div></div>");
    p += F("<div class=\"ro\"><div class=\"val\">"); p += fmtVol(t.ml); p += F("</div><div class=\"unit\">");
    p += volUnit(); p += F(" volume</div></div>");
    p += F("<div class=\"ro big\"><div class=\"val\">"); p += fmtMass(t.g); p += F("</div><div class=\"unit\">");
    p += massUnit(); p += F(" mass</div></div>");
    p += F("</div>");

    if (t.targetG) {
      uint32_t pct = t.g >= t.targetG ? 100 : (t.g * 100UL) / t.targetG;
      p += F("<div class=\"target\"><span>Target ");
      p += fmtMass(t.targetG);
      p += ' ';
      p += massUnit();
      p += F("</span><span>");
      p += pct;
      p += F("%</span></div><div class=\"bar\"><div style=\"width:");
      p += pct;
      p += F("%\"></div></div>");
    }
    p += foot;
    sendNoCache();
    server.send(200, "text/html", p);
  }

  // ---------- /start /stop ----------
  void handleStart() {
    const Telemetry& t = UnoLink::telemetry();
    float v = server.arg("kg").toFloat();
    float kg = inputToKg(v);
    if (!(kg >= TARGET_MIN_KG && kg <= TARGET_MAX_KG)) return redirect("/?m=badkg");
#if !FAKE_UNO
    if (!t.linkOk()) return redirect("/?m=nolink");
#endif
    if (!t.idle()) return redirect("/?m=busy");
    UnoLink::sendStart(kg);
    redirect("/?m=started");
  }
  void handleStop() {
    UnoLink::sendStop();
    redirect("/?m=stopped");
  }

  // ---------- /history ----------
  void handleHistory() {
    static SessionRow rows[HISTORY_ROWS];
    size_t n = Store::recentSessions(rows, HISTORY_ROWS);
    String p = head("History");
    p.reserve(1200 + n * 170);
    p += nav("History");
    p += F("<main><div class=\"row\"><h1>Sessions</h1><a class=\"btn small\" href=\"/export.csv\">Export CSV</a></div>");
    if (!n) {
      p += F("<p class=\"hint\">No sessions yet. Completed fills appear here.</p>");
    } else {
      p += F("<div class=\"tablewrap\"><table><thead><tr><th>#</th><th>Time</th><th>Target</th><th>Volume</th><th>Mass</th><th>Density</th><th>End</th></tr></thead><tbody>");
      for (size_t i = 0; i < n; i++) {
        const SessionRow& r = rows[i];
        p += F("<tr><td><a href=\"/slip?id=");
        p += r.id; p += F("\">"); p += r.id; p += F("</a></td><td>");
        String ts = Store::formatTime(r.epoch);
        p += ts.length() ? ts : String("<i>not set</i>");
        p += F("</td><td>"); p += fmtMass(r.targetG, 2); p += ' '; p += massUnit();
        p += F("</td><td>"); p += fmtVol(r.ml); p += ' '; p += volUnit();
        p += F("</td><td>"); p += fmtMass(r.g); p += ' '; p += massUnit();
        p += F("</td><td>"); p += fmtDens(r.densGml);
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

  // ---------- /slip ----------
  void slipLine(String& p, const char* k, const String& v) {
    p += F("<div class=\"ln\"><span>"); p += k; p += F("</span><b>"); p += v; p += F("</b></div>");
  }
  void handleSlip() {
    uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
    SessionRow r;
    if (!id || !Store::findSession(id, r)) { server.send(404, "text/plain", "No such session"); return; }
    String p = head("Fuel slip");
    p.reserve(2600);
    p += nav("");
    p += F("<main class=\"slip\"><div class=\"ticket\"><div class=\"tk-head\"><div class=\"tk-brand\">FlyPrecise</div><div class=\"tk-sub\">Digital fuel slip</div></div>");
    slipLine(p, "Session", String("#") + r.id);
    String ts = Store::formatTime(r.epoch);
    slipLine(p, "Date / time", ts.length() ? ts : String("(time not set)"));
    slipLine(p, "Target", fmtMass(r.targetG, 2) + " " + massUnit());
    p += F("<hr>");
    slipLine(p, "Delivered volume", String(r.ml / 1000.0f, 2) + " L &nbsp;/&nbsp; " + String(r.ml * ML_TO_GAL, 3) + " gal");
    slipLine(p, "Density", r.densGml < 0 ? String("--") :
             String(r.densGml, 3) + " g/mL &nbsp;/&nbsp; " + String(r.densGml * GML_TO_LBGAL, 2) + " lb/gal");
    slipLine(p, "Delivered mass", String(r.g / 1000.0f, 3) + " kg &nbsp;/&nbsp; " + String(r.g * G_TO_LB, 3) + " lb");
    slipLine(p, "Stopped by", String(r.reason));
    p += F("<hr><div class=\"fill\"><span>Aircraft tail #</span><em></em></div>"
           "<div class=\"fill\"><span>Pilot</span><em></em></div>"
           "<div class=\"fill\"><span>Signature</span><em></em></div>"
           "<div class=\"tk-foot\">Measured by flow meter and in-line densimeter. Not a legal-for-trade instrument.</div>"
           "</div><p class=\"hint noprint\">Print: phone share menu &rarr; Print, or Ctrl+P. "
           "<a href=\"/history\">Back to history</a></p></main>");
    p += foot;
    sendNoCache();
    server.send(200, "text/html", p);
  }

  // ---------- /export.csv ----------
  void handleExport() {
    File f = LittleFS.open(SESSIONS_PATH, "r");
    if (!f) { server.send(404, "text/plain", "no sessions file"); return; }
    server.sendHeader("Content-Disposition", "attachment; filename=\"flyprecise_sessions.csv\"");
    sendNoCache();
    server.streamFile(f, "text/csv");
    f.close();
  }

  // ---------- /settings ----------
  // days-from-civil (Howard Hinnant). Avoids depending on timegm().
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
    const Settings& s = Store::settings();
    String p = head("Settings");
    p.reserve(2600);
    p += nav("Settings");
    p += F("<main><h1>Settings</h1><form method=\"post\" action=\"/settings\" class=\"settings\">");
    p += F("<fieldset><legend>Clock</legend><p class=\"hint\">The ESP32 has no battery clock. Set this once per power-up so slips get a date.</p>"
           "<label for=\"dt\">Current date &amp; time</label><input id=\"dt\" name=\"dt\" type=\"datetime-local\" value=\"");
    p += Store::formatTimeForInput(Store::nowEpoch());
    p += F("\"></fieldset>");
    p += F("<fieldset><legend>Units</legend><div class=\"opts\">");
    radio(p, "um", "kg", "kg", !s.massLb());
    radio(p, "um", "lb", "lb", s.massLb());
    p += F("</div><div class=\"opts\">");
    radio(p, "uv", "L", "litres (density in g/mL)", !s.volGal());
    radio(p, "uv", "gal", "gallons (density in lb/gal)", s.volGal());
    p += F("</div><p class=\"hint\">Slips always show both.</p></fieldset>");
    p += F("<fieldset><legend>Density calibration</legend><p class=\"hint\">corrected = a &times; raw + b. Sent to the Uno as <code>C&lt;a&gt;,&lt;b&gt;</code>. Leave at 1 / 0 until the Oct 19 calibration is done.</p>"
           "<label for=\"ca\">a</label><input id=\"ca\" name=\"ca\" type=\"number\" step=\"any\" value=\"");
    p += String(s.calA, 5);
    p += F("\"><label for=\"cb\">b</label><input id=\"cb\" name=\"cb\" type=\"number\" step=\"any\" value=\"");
    p += String(s.calB, 5);
    p += F("\"></fieldset>");
    p += F("<fieldset><legend>Wi-Fi</legend><label for=\"ap\">Hotspot password (8+ chars, applies after reboot; blank = keep)</label>"
           "<input id=\"ap\" name=\"ap\" type=\"text\" minlength=\"8\" maxlength=\"32\" autocomplete=\"off\"></fieldset>");
    p += F("<button class=\"btn start\" type=\"submit\">Save</button></form>");
    p += F("<p class=\"hint\">Storage: ");
    p += Store::storageOk() ? "OK" : "FAILED";
    p += F(", sessions.csv ");
    p += (unsigned)Store::sessionsFileSize();
    p += F(" bytes. <a href=\"/export.csv\">Export</a></p></main>");
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
      if (e != Store::nowEpoch()) Store::setClock(e);   // unchanged form value = don't touch the running clock
    }
    String um = server.arg("um"), uv = server.arg("uv");
    if (um == "kg" || um == "lb") strlcpy(s.unitsMass, um.c_str(), sizeof(s.unitsMass));
    if (uv == "L" || uv == "gal") strlcpy(s.unitsVol, uv.c_str(), sizeof(s.unitsVol));
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
    p.reserve(1800);
    p += nav("");
    p += F("<main><h1>Advanced</h1><div class=\"banner err\">Manual valve and pump control. Toggling anything here cancels the automatic fill sequence on the Uno. "
           "Opening the pump with both valves closed dead-heads it.</div>");
    p += F("<div class=\"adv\">");
    const char* cmds[][2] = { {"P", "Pump"}, {"B", "Bypass valve"}, {"M", "Mainline valve"} };
    bool st[3] = { t.pump(), t.bypass(), t.mainline() };
    for (int i = 0; i < 3; i++) {
      p += F("<form method=\"post\" action=\"/advanced\"><input type=\"hidden\" name=\"cmd\" value=\"");
      p += cmds[i][0];
      p += F("\"><button class=\"btn adv-btn\" type=\"submit\"><span>");
      p += cmds[i][1];
      p += F("</span><b class=\"state ");
      p += st[i] ? "on\">ON" : "\">OFF";
      p += F("</b></button></form>");
    }
    p += F("</div><form method=\"post\" action=\"/stop\"><button class=\"btn stop\" type=\"submit\">STOP ALL</button></form>");
    p += F("<p class=\"hint\">Last Uno event: <code>");
    p += UnoLink::lastEvent();
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
    p.reserve(500);
    p += "FlyPrecise ESP32 debug\n";
    p += "fake_uno: "; p += FAKE_UNO; p += '\n';
    p += "uptime_s: "; p += millis() / 1000; p += '\n';
    p += "free_heap: "; p += ESP.getFreeHeap(); p += '\n';
    p += "ap_clients: "; p += WiFi.softAPgetStationNum(); p += '\n';
    p += "link_ok: "; p += t.linkOk(); p += "  last_rx_ms_ago: "; p += t.everReceived ? millis() - t.lastRxMs : 0; p += '\n';
    p += "phase: "; p += phaseName(t.phase); p += "  flags: "; p += t.flags; p += '\n';
    p += "dens_gml: "; p += String(t.densGml, 3); p += "  ml: "; p += t.ml; p += "  g: "; p += t.g;
    p += "  target_g: "; p += t.targetG; p += "  dist_mm: "; p += t.distMm; p += '\n';
    p += "last_event: "; p += UnoLink::lastEvent(); p += '\n';
    p += "clock_set: "; p += Store::clockSet(); p += "  epoch: "; p += Store::nowEpoch(); p += '\n';
    p += "sessions_bytes: "; p += (unsigned)Store::sessionsFileSize(); p += "  next_id: "; p += Store::settings().nextId; p += '\n';
    sendNoCache();
    server.send(200, "text/plain", p);
  }

  // ---------- static ----------
  void sendEmbedded(const uint8_t* s, const uint8_t* e, const char* type, bool cache) {
    size_t len = e - s;
    if (len && s[len - 1] == '\0') len--;   // embed_txtfiles appends a NUL
    server.sendHeader("Cache-Control", cache ? "max-age=86400" : "no-store");
    server.send_P(200, type, (const char*)s, len);
  }
  void handleCss()      { sendEmbedded(style_css_start, style_css_end, "text/css", true); }
  void handleManifest() { sendEmbedded(manifest_json_start, manifest_json_end, "application/manifest+json", true); }
  void handleIcon()     { sendEmbedded(icon_svg_start, icon_svg_end, "image/svg+xml", true); }

  // Captive-portal probes and anything unknown -> the dashboard.
  void handleNotFound() { redirect(String("http://") + AP_IP.toString() + "/"); }
}

void Web::noteSessionSaved(uint32_t id) { savedId = id; savedAtMs = millis(); }

void Web::begin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/live", HTTP_GET, handleLive);
  server.on("/start", HTTP_POST, handleStart);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/history", HTTP_GET, handleHistory);
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
  // OS connectivity probes. Returning a redirect makes phones pop the portal.
  const char* probes[] = { "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                           "/connecttest.txt", "/ncsi.txt", "/success.txt", "/canonical.html", "/redirect" };
  for (auto u : probes) server.on(u, handleNotFound);
  server.onNotFound(handleNotFound);
  server.begin();
}

void Web::loop() { server.handleClient(); }
