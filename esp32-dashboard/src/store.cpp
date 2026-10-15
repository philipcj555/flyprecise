#include "store.h"
#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <sys/time.h>

namespace {
  Settings cfg;
  bool fsOk = false;
  bool clockIsSet = false;

  bool parseRow(char* line, SessionRow& r) {
    // id,epoch,target_g,delivered_ml,delivered_g,density_gml,reason
    char* f[8];
    size_t n = 0;
    char* p = line;
    while (p && n < 8) {
      f[n++] = p;
      char* c = strchr(p, ',');
      if (!c) break;
      *c = '\0';
      p = c + 1;
    }
    if (n < 7 || !isdigit((unsigned char)f[0][0])) return false;   // header or junk
    r.id = strtoul(f[0], nullptr, 10);
    r.epoch = strtoul(f[1], nullptr, 10);
    r.targetG = strtoul(f[2], nullptr, 10);
    r.ml = strtoul(f[3], nullptr, 10);
    r.g = strtoul(f[4], nullptr, 10);
    r.densGml = atof(f[5]);
    strlcpy(r.reason, f[6], sizeof(r.reason));
    size_t rl = strlen(r.reason);
    while (rl && (r.reason[rl - 1] == '\r' || r.reason[rl - 1] == '\n')) r.reason[--rl] = '\0';
    return true;
  }

  void ensureSessionsFile() {
    if (!LittleFS.exists(SESSIONS_PATH)) {
      File f = LittleFS.open(SESSIONS_PATH, "w");
      if (f) { f.print(SESSIONS_CSV_HEADER); f.close(); }
    }
  }

  bool loadSettings() {
    File f = LittleFS.open(SETTINGS_PATH, "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { Serial.printf("[store] settings.json parse error: %s\n", err.c_str()); return false; }
    cfg.nextId = doc["next_id"] | 1;
    strlcpy(cfg.unitsMass, doc["units_mass"] | "kg", sizeof(cfg.unitsMass));
    strlcpy(cfg.unitsVol,  doc["units_vol"]  | "L",  sizeof(cfg.unitsVol));
    cfg.calA = doc["cal_a"] | 1.0f;
    cfg.calB = doc["cal_b"] | 0.0f;
    strlcpy(cfg.apPass, doc["ap_pass"] | "", sizeof(cfg.apPass));
    return true;
  }

  // Iterate rows calling fn(row). Stops early if fn returns false.
  template <typename F>
  void forEachRow(F fn) {
    File f = LittleFS.open(SESSIONS_PATH, "r");
    if (!f) return;
    char line[128];
    while (f.available()) {
      size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
      line[n] = '\0';
      SessionRow r;
      if (parseRow(line, r) && !fn(r)) break;
    }
    f.close();
  }
}

bool Store::begin() {
  fsOk = LittleFS.begin(true /* format on fail */);
  if (!fsOk) { Serial.println("[store] LittleFS mount failed"); return false; }
  if (!loadSettings()) { Serial.println("[store] creating default settings.json"); saveSettings(); }
  ensureSessionsFile();
  Serial.printf("[store] ok. next_id=%lu sessions.csv=%u bytes\n", (unsigned long)cfg.nextId, (unsigned)sessionsFileSize());
  return true;
}

Settings& Store::settings() { return cfg; }
bool Store::storageOk() { return fsOk; }

bool Store::saveSettings() {
  if (!fsOk) return false;
  JsonDocument doc;
  doc["next_id"] = cfg.nextId;
  doc["units_mass"] = cfg.unitsMass;
  doc["units_vol"] = cfg.unitsVol;
  doc["cal_a"] = cfg.calA;
  doc["cal_b"] = cfg.calB;
  doc["ap_pass"] = cfg.apPass;
  File f = LittleFS.open(SETTINGS_PATH, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

size_t Store::sessionsFileSize() {
  File f = LittleFS.open(SESSIONS_PATH, "r");
  if (!f) return 0;
  size_t s = f.size();
  f.close();
  return s;
}

uint32_t Store::appendSession(const StopEvent& ev) {
  if (!fsOk) return 0;
  if (sessionsFileSize() > SESSIONS_MAX_BYTES) {
    LittleFS.remove(SESSIONS_ROTATED);
    LittleFS.rename(SESSIONS_PATH, SESSIONS_ROTATED);
    ensureSessionsFile();
  }
  uint32_t id = cfg.nextId++;
  saveSettings();   // persist the counter before the row so ids never repeat

  File f = LittleFS.open(SESSIONS_PATH, "a");
  if (!f) return 0;
  char line[128];
  snprintf(line, sizeof(line), "%lu,%lu,%lu,%lu,%lu,%.3f,%s\n",
           (unsigned long)id, (unsigned long)nowEpoch(), (unsigned long)ev.targetG,
           (unsigned long)ev.ml, (unsigned long)ev.g, ev.densGml, ev.reason);
  f.print(line);
  f.close();
  Serial.printf("[store] session %lu: %s", (unsigned long)id, line);
  return id;
}

size_t Store::recentSessions(SessionRow* out, size_t max) {
  // Ring buffer of the last `max` rows in file order, then reverse into `out`.
  size_t count = 0, head = 0;
  forEachRow([&](const SessionRow& r) {
    out[head] = r;
    head = (head + 1) % max;
    if (count < max) count++;
    return true;
  });
  // out[] currently holds the last `count` rows in a ring ending just before `head`.
  // Rotate into a temp order: newest first.
  SessionRow tmp[HISTORY_ROWS];
  for (size_t i = 0; i < count; i++) {
    size_t idx = (head + max - 1 - i) % max;
    tmp[i] = out[idx];
  }
  for (size_t i = 0; i < count; i++) out[i] = tmp[i];
  return count;
}

bool Store::findSession(uint32_t id, SessionRow& out) {
  bool found = false;
  forEachRow([&](const SessionRow& r) {
    if (r.id == id) { out = r; found = true; return false; }
    return true;
  });
  return found;
}

// ---- clock ----
bool Store::clockSet() { return clockIsSet; }

void Store::setClock(time_t epoch) {
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  clockIsSet = true;
}

uint32_t Store::nowEpoch() {
  return clockIsSet ? (uint32_t)time(nullptr) : 0;
}

// The datetime-local form gives wall-clock local time with no zone; we store
// it as-is and format it back with gmtime so it round-trips unchanged.
String Store::formatTime(uint32_t epoch) {
  if (!epoch) return String();
  time_t t = epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tmv);
  return String(b);
}

String Store::formatTimeForInput(uint32_t epoch) {
  if (!epoch) return String();
  time_t t = epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M", &tmv);
  return String(b);
}
