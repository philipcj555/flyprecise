#pragma once
#include <Arduino.h>
#include <time.h>
#include "uno_link.h"

struct Settings {
  uint32_t nextId = 1;
  char unitsMass[4] = "lb";    // "kg" | "lb"
  char unitsVol[4]  = "gal";   // "L" | "gal"
  float nominalLbGal = 6.7f;   // planning density airlines assume when they don't measure
  float calA = 1.0f;
  float calB = 0.0f;
  char apPass[33] = "";        // empty -> AP_PASS_DEFAULT

  bool massLb() const { return unitsMass[0] == 'l'; }
  bool volGal() const { return unitsVol[0] == 'g'; }
};

// What the operator types in before pressing START. Travels with the session.
struct FlightInfo {
  char flight[12] = "";        // "XX1234"
  char origin[6] = "";         // "LAX"
  char dest[6] = "";           // "SIN"
  char aircraft[16] = "";      // "B787-900"
  bool set() const { return flight[0] != '\0'; }
};

struct SessionRow {
  uint32_t id = 0;
  uint32_t epoch = 0;          // 0 = clock was not set when recorded
  FlightInfo flight;
  uint32_t targetG = 0;
  uint32_t ml = 0;
  uint32_t g = 0;
  float    densGml = -1.0f;
  char     reason[8] = "";
};

struct Stats {
  uint32_t flights = 0;
  uint32_t flightsWithDensity = 0;
  double   totalMl = 0;
  double   totalG = 0;
  double   totalDiffLb = 0;    // sum of FlyPrecise difference over flights with a valid density
  double   sumDensGml = 0;     // for the average
};

namespace Store {
  bool begin();                              // mount (format on first boot), load settings
  Settings& settings();
  bool saveSettings();

  uint32_t appendSession(const StopEvent& ev, const FlightInfo& fi);   // returns the new session id
  // Fills `out` with up to `max` rows, newest first. Returns count.
  size_t recentSessions(SessionRow* out, size_t max);
  bool findSession(uint32_t id, SessionRow& out);
  bool lastSession(SessionRow& out);
  void computeStats(Stats& s);
  size_t sessionsFileSize();
  bool storageOk();

  // Clock. ESP32 has no battery RTC; the user sets it from /settings each boot.
  bool clockSet();
  void setClock(time_t epoch);
  uint32_t nowEpoch();                       // 0 if clock not set
  String formatTime(uint32_t epoch);         // "2026-11-02 14:05" or ""
  String formatTimeForInput(uint32_t epoch); // "2026-11-02T14:05" for <input type=datetime-local>

  // FlyPrecise difference in lb: how much more fuel a nominal-density plan would
  // have loaded for the same requested mass. Negative = less fuel carried.
  // Returns false when the density is not valid.
  bool differenceLb(uint32_t targetG, float densGml, float& outLb);
}
