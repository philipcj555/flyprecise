#pragma once
#include <Arduino.h>
#include <time.h>
#include "uno_link.h"

struct Settings {
  uint32_t nextId = 1;
  char unitsMass[4] = "kg";    // "kg" | "lb"
  char unitsVol[4]  = "L";     // "L" | "gal"
  float calA = 1.0f;
  float calB = 0.0f;
  char apPass[33] = "";        // empty -> AP_PASS_DEFAULT

  bool massLb() const { return unitsMass[0] == 'l'; }
  bool volGal() const { return unitsVol[0] == 'g'; }
};

struct SessionRow {
  uint32_t id = 0;
  uint32_t epoch = 0;          // 0 = clock was not set when recorded
  uint32_t targetG = 0;
  uint32_t ml = 0;
  uint32_t g = 0;
  float    densGml = -1.0f;
  char     reason[8] = "";
};

namespace Store {
  bool begin();                              // mount (format on first boot), load settings
  Settings& settings();
  bool saveSettings();

  uint32_t appendSession(const StopEvent& ev);   // returns the new session id
  // Fills `out` with up to `max` rows, newest first. Returns count.
  size_t recentSessions(SessionRow* out, size_t max);
  bool findSession(uint32_t id, SessionRow& out);
  size_t sessionsFileSize();
  bool storageOk();

  // Clock. ESP32 has no battery RTC; the user sets it from /settings each boot.
  bool clockSet();
  void setClock(time_t epoch);
  uint32_t nowEpoch();                       // 0 if clock not set
  String formatTime(uint32_t epoch);         // "2026-11-02 14:05" or ""
  String formatTimeForInput(uint32_t epoch); // "2026-11-02T14:05" for <input type=datetime-local>
}
