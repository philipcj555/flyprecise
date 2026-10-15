#include "uno_link.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>

const char* phaseName(Phase p) {
  switch (p) {
    case Phase::IDLE:    return "IDLE";
    case Phase::PRIMING: return "PRIMING";
    case Phase::OVERLAP: return "OVERLAP";
    case Phase::RUNNING: return "RUNNING";
    default:             return "NO LINK";
  }
}

bool Telemetry::linkOk() const {
  return everReceived && (millis() - lastRxMs) < UNO_LINK_TIMEOUT_MS;
}

#if FAKE_UNO
void fakeCommand(const char* s);   // defined below
#endif

namespace {
  Telemetry tele;
#if !FAKE_UNO
  char lineBuf[96];
  size_t lineLen = 0;
#endif
  char lastEventBuf[96] = "";
  char lastErrorBuf[64] = "";
  uint32_t lastEventAt = 0;
  uint32_t lastErrorAt = 0;
  uint32_t sessionTargetG = 0;
  UnoLink::StopHandler stopHandler = nullptr;

  Phase parsePhase(const char* s) {
    if (!strcmp(s, "IDLE"))    return Phase::IDLE;
    if (!strcmp(s, "PRIMING")) return Phase::PRIMING;
    if (!strcmp(s, "OVERLAP")) return Phase::OVERLAP;
    if (!strcmp(s, "RUNNING")) return Phase::RUNNING;
    return Phase::UNKNOWN;
  }

  // Split a comma-separated line in place. Returns the field count.
  size_t splitFields(char* s, char** out, size_t maxFields) {
    size_t n = 0;
    char* p = s;
    while (p && n < maxFields) {
      out[n++] = p;
      char* c = strchr(p, ',');
      if (!c) break;
      *c = '\0';
      p = c + 1;
    }
    return n;
  }

  void handleTelemetry(char** f, size_t n) {
    // $T,<phase>,<dens>,<ml>,<g>,<target_g>,<dist_mm>,<flags>
    if (n < 8) return;
    tele.phase   = parsePhase(f[1]);
    tele.densGml = atof(f[2]);
    tele.ml      = strtoul(f[3], nullptr, 10);
    tele.g       = strtoul(f[4], nullptr, 10);
    tele.targetG = strtoul(f[5], nullptr, 10);
    tele.distMm  = strtoul(f[6], nullptr, 10);
    tele.flags   = (uint8_t)strtoul(f[7], nullptr, 10);
    tele.lastRxMs = millis();
    tele.everReceived = true;
  }

  void handleEvent(char** f, size_t n, const char* raw) {
    if (n < 2) return;
    strlcpy(lastEventBuf, raw, sizeof(lastEventBuf));
    lastEventAt = millis();

    if (!strcmp(f[1], "START") && n >= 3) {
      sessionTargetG = strtoul(f[2], nullptr, 10);
    } else if (!strcmp(f[1], "STOP") && n >= 6) {
      StopEvent ev;
      ev.ml = strtoul(f[2], nullptr, 10);
      ev.g = strtoul(f[3], nullptr, 10);
      ev.densGml = atof(f[4]);
      strlcpy(ev.reason, f[5], sizeof(ev.reason));
      ev.targetG = sessionTargetG ? sessionTargetG : tele.targetG;
      sessionTargetG = 0;
      if (stopHandler) stopHandler(ev);
    } else if (!strcmp(f[1], "ERR")) {
      strlcpy(lastErrorBuf, n >= 3 ? f[2] : "unknown", sizeof(lastErrorBuf));
      lastErrorAt = millis();
    } else if (!strcmp(f[1], "BOOT")) {
      sessionTargetG = 0;
    }
  }

  void writeToUno(const char* s) {
#if FAKE_UNO
    fakeCommand(s);
#else
    Serial1.print(s);
#endif
    Serial.printf("[uno<-] %s", s);
  }
}

void UnoLink::handleLine(char* line) {
  // Strip trailing CR/LF and whitespace.
  size_t len = strlen(line);
  while (len && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) line[--len] = '\0';
  if (len < 2 || line[0] != '$') return;   // ignore debug prints / bootloader noise

  char raw[96];
  strlcpy(raw, line, sizeof(raw));
  char* f[10];
  size_t n = splitFields(line, f, 10);
  if (!strcmp(f[0], "$T")) handleTelemetry(f, n);
  else if (!strcmp(f[0], "$E")) handleEvent(f, n, raw);
}

// ---------------------------------------------------------------------------
// FAKE_UNO: simulated rig so the whole UI can be built without hardware.
// Generates the same $T/$E lines the real Uno would, and feeds them through
// the same parser, so the parser is exercised too.
// ---------------------------------------------------------------------------
#if FAKE_UNO
namespace {
  Phase    simPhase = Phase::IDLE;
  uint32_t simPhaseStart = 0;
  uint32_t simLastTick = 0;
  uint32_t simLastTele = 0;
  float    simMl = 0;
  uint32_t simTargetG = 0;
  const float SIM_DENS_GML = 0.815f;        // ~6.8 lb/gal
  const float SIM_FLOW_ML_S = 130.0f;       // ~7.8 L/min at full flow

  uint8_t simFlags() {
    uint8_t f = 0;
    if (simPhase != Phase::IDLE) f |= FLAG_PUMP;
    if (simPhase == Phase::PRIMING || simPhase == Phase::OVERLAP) f |= FLAG_BYPASS;
    if (simPhase == Phase::OVERLAP || simPhase == Phase::RUNNING) f |= FLAG_MAINLINE;
    return f;
  }

  void simEmitStop(const char* reason) {
    char l[80];
    uint32_t g = (uint32_t)(simMl * SIM_DENS_GML);
    snprintf(l, sizeof(l), "$E,STOP,%lu,%lu,%.3f,%s", (unsigned long)simMl, (unsigned long)g, SIM_DENS_GML, reason);
    UnoLink::handleLine(l);
    simPhase = Phase::IDLE;
  }
}

void fakeCommand(const char* s) {
  if (s[0] == 'R' && simPhase == Phase::IDLE) {
    float kg = atof(s + 1);
    simTargetG = (uint32_t)(kg * 1000.0f);
    simMl = 0;
    simPhase = Phase::PRIMING;
    simPhaseStart = millis();
    char l[48];
    snprintf(l, sizeof(l), "$E,START,%lu", (unsigned long)simTargetG);
    UnoLink::handleLine(l);
  } else if (s[0] == 'S') {
    if (simPhase != Phase::IDLE) simEmitStop("MANUAL");
  }
  // P/B/M are accepted and ignored by the simulator.
}

static void fakePoll() {
  uint32_t now = millis();
  if (simLastTick == 0) { simLastTick = now; UnoLink::handleLine((char*)"$E,BOOT"); }
  float dt = (now - simLastTick) / 1000.0f;
  simLastTick = now;

  if (simPhase == Phase::PRIMING) {
    simMl += SIM_FLOW_ML_S * 0.3f * dt;                       // bypass: counts a little flow
    if (now - simPhaseStart >= 10000) { simPhase = Phase::OVERLAP; simPhaseStart = now; }
  } else if (simPhase == Phase::OVERLAP) {
    simMl += SIM_FLOW_ML_S * 0.6f * dt;
    if (now - simPhaseStart >= 150) { simPhase = Phase::RUNNING; simPhaseStart = now; }
  } else if (simPhase == Phase::RUNNING) {
    simMl += SIM_FLOW_ML_S * dt;
    if (simMl * SIM_DENS_GML >= (float)simTargetG - 50.0f) simEmitStop("TARGET");
  }

  if (now - simLastTele >= 500) {
    simLastTele = now;
    char l[96];
    float dens = (simPhase == Phase::RUNNING || simPhase == Phase::IDLE) ? SIM_DENS_GML : -1.0f;
    snprintf(l, sizeof(l), "$T,%s,%.3f,%lu,%lu,%lu,%u,%u",
             phaseName(simPhase), dens, (unsigned long)simMl,
             (unsigned long)(simMl * SIM_DENS_GML), (unsigned long)simTargetG,
             (unsigned)(140 + (now / 700) % 5), simFlags());
    UnoLink::handleLine(l);
  }
}
#endif

// ---------------------------------------------------------------------------

void UnoLink::begin() {
#if !FAKE_UNO
  Serial1.begin(UNO_UART_BAUD, SERIAL_8N1, UNO_UART_RX_PIN, UNO_UART_TX_PIN);
#endif
}

void UnoLink::poll() {
#if FAKE_UNO
  fakePoll();
#else
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      handleLine(lineBuf);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;   // overlong garbage, drop it
    }
  }
#endif
}

const Telemetry& UnoLink::telemetry() { return tele; }
const char* UnoLink::lastEvent() { return lastEventBuf; }
uint32_t UnoLink::lastEventMs() { return lastEventAt; }
uint32_t UnoLink::lastErrorMs() { return lastErrorAt; }
const char* UnoLink::lastError() { return lastErrorBuf; }
void UnoLink::onStop(StopHandler h) { stopHandler = h; }

void UnoLink::sendStart(float kg) {
  char b[16];
  snprintf(b, sizeof(b), "R%.2f\n", kg);
  writeToUno(b);
}
void UnoLink::sendStop() { writeToUno("S\n"); }
void UnoLink::sendRaw(const char* cmd) {
  char b[8];
  snprintf(b, sizeof(b), "%c\n", cmd[0]);
  writeToUno(b);
}
void UnoLink::sendCalibration(float a, float b) {
  char buf[32];
  snprintf(buf, sizeof(buf), "C%.5f,%.5f\n", a, b);
  writeToUno(buf);
}
