#pragma once
#include <Arduino.h>

// Serial protocol with the Uno. See docs/serial_protocol.md.
//
//   Uno -> ESP32   $T,<phase>,<dens_gml>,<ml>,<g>,<target_g>,<dist_mm>,<flags>   every 500 ms
//                  $E,START,<target_g>
//                  $E,STOP,<ml>,<g>,<dens_gml>,<reason>
//                  $E,ERR,<text>
//                  $E,BOOT
//   ESP32 -> Uno   R<kg>\n  S\n  P\n  B\n  M\n  C<a>,<b>\n

enum class Phase : uint8_t { IDLE, PRIMING, OVERLAP, RUNNING, UNKNOWN };
const char* phaseName(Phase p);

enum TelemetryFlags : uint8_t {
  FLAG_PUMP        = 1 << 0,
  FLAG_BYPASS      = 1 << 1,
  FLAG_MAINLINE    = 1 << 2,
  FLAG_TOF_TIMEOUT = 1 << 3,
  FLAG_SENSOR_ERR  = 1 << 4,
};

struct Telemetry {
  Phase    phase = Phase::UNKNOWN;
  float    densGml = -1.0f;   // g/mL; -1 = not valid yet (uncalibrated / not settled)
  uint32_t ml = 0;
  uint32_t g = 0;
  uint32_t targetG = 0;
  uint32_t distMm = 0;
  uint8_t  flags = 0;
  uint32_t lastRxMs = 0;
  bool     everReceived = false;

  bool pump() const        { return flags & FLAG_PUMP; }
  bool bypass() const      { return flags & FLAG_BYPASS; }
  bool mainline() const    { return flags & FLAG_MAINLINE; }
  bool tofTimeout() const  { return flags & FLAG_TOF_TIMEOUT; }
  bool sensorError() const { return flags & FLAG_SENSOR_ERR; }
  bool linkOk() const;
  bool idle() const        { return phase == Phase::IDLE; }
};

struct StopEvent {
  uint32_t ml = 0;
  uint32_t g = 0;
  float    densGml = -1.0f;
  uint32_t targetG = 0;   // from the matching $E,START (or last telemetry)
  char     reason[8] = "MANUAL";
};

namespace UnoLink {
  typedef void (*StopHandler)(const StopEvent&);

  void begin();
  void poll();                       // call from loop(); reads UART or runs the simulator
  const Telemetry& telemetry();
  const char* lastEvent();           // last raw $E line (for the debug page / log)
  uint32_t lastEventMs();
  uint32_t lastErrorMs();
  const char* lastError();           // text of the last $E,ERR
  void onStop(StopHandler h);

  void sendStart(float kg);
  void sendStop();
  void sendRaw(const char* cmd);     // "P", "B", "M" (advanced page)
  void sendCalibration(float a, float b);

  // Exposed for unit-style testing of the parser.
  void handleLine(char* line);
}
