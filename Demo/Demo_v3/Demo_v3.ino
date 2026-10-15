// FlyPrecise Uno firmware v3 = DemoFinal.ino + ESP32 dashboard link.
// Protocol: docs/serial_protocol.md. Plan: docs/FLYPRECISE_DASHBOARD_PLAN.md (section 5.4).
//
// Changes from DemoFinal.ino:
//   * Debug Serial.println spam removed. Serial now carries the $T/$E protocol
//     (plus nothing else), so the ESP32 on D0/D1 and the Arduino serial monitor
//     both see clean lines.
//   * Commands are line-based and non-blocking: "R2.5\n" sets target and starts.
//     Bare "S"/"P"/"B"/"M" still work from the serial monitor (100 ms idle = end of line).
//   * BUG FIX: DemoFinal compared totalGrams (g) against targetkg (kg) - 50, so the
//     stop condition was effectively always true. Target is now held in grams.
//   * Totals reset at the start of each fill, so $E,STOP reports what THIS fill
//     delivered (DemoFinal accumulated across fills via currentkg).
//   * ToF read is non-blocking (polls the data-ready bit) so a STOP from the
//     dashboard is never delayed by the sensor's 500 ms timeout.
//   * Sensor init failure no longer halts the board forever; it sets flag bit4
//     and the fill logic (which never used distance) keeps working.
//   * LCD output is unchanged.
#include <LiquidCrystal.h>
#include <Wire.h>
#include <VL53L0X.h>

LiquidCrystal lcd(12, 11, 5, 4, 3, 8);
/*
LCD Pins:   VSS GND, VDD 5V, V0 pot, RS D12, RW GND, E D11, D4-7 = D5,D4,D3,D8, A res->5V, K GND
VL53L0X:    5V, GND, SCL A5, SDA A4
Flow meter: 5V, GND, yellow D2 (interrupt)
MOSFETs:    Pump D6, Bypass D9, Mainline D10
ESP32 link: D1 (TX) -> divider -> ESP32 RX ;  ESP32 TX -> D0 (RX)   (see plan section 4.4)
*/
VL53L0X sensor;

const byte flowPin = 2;
const byte pumpPin = 6;
const byte bypassPin = 9;
const byte mainlinePin = 10;

const float calibrationFactor = 17.0f;
const unsigned long flowPeriodMs = 1000;
const unsigned long primeTimeMs = 10000;
const unsigned long overlapTimeMs = 150;
const unsigned long telemetryPeriodMs = 500;
const unsigned long tofStaleMs = 500;          // no fresh ToF sample for this long = timeout flag
const unsigned long cmdIdleMs = 100;           // bytes with no newline: treat as a line after this
const unsigned long stopMarginGrams = 50;      // stop this many grams early (the "-50" in DemoFinal)
const float targetMinKg = 0.1f;
const float targetMaxKg = 50.0f;

// Density. Still the hardcoded placeholder until the Oct 19 calibration lands.
// When that is done: set DENSITY_CALIBRATED = true and make densityRaw() return
// the uncorrected measurement; calA/calB then apply (also settable with "C<a>,<b>").
const float densityPlaceholderGml = 1.02f;
const bool  DENSITY_CALIBRATED = false;
float calA = 1.0f;
float calB = 0.0f;

volatile unsigned long pulseCount = 0;

unsigned long lastFlowTime = 0;
unsigned long lastDisplayTime = 0;
unsigned long lastTelemetryTime = 0;
unsigned long lastDistanceOkTime = 0;
unsigned long totalMilliLitres = 0;
unsigned long totalGrams = 0;
float fractionalMilliLitres = 0;

unsigned long targetGrams = 3000;

bool pumpState = false;
bool bypassState = false;
bool mainlineState = false;

enum Phase { IDLE, PRIMING, OVERLAP, RUNNING };
Phase phase = IDLE;
unsigned long phaseStartedAt = 0;

uint16_t rawDistance = 0;
bool distanceTimedOut = false;
bool sensorInitError = false;

char cmdBuf[24];
byte cmdLen = 0;
unsigned long cmdLastByteAt = 0;

// ---------------------------------------------------------------- helpers
const char* phaseName() {
  switch (phase) {
    case PRIMING: return "PRIMING";
    case OVERLAP: return "OVERLAP";
    case RUNNING: return "RUNNING";
    default:      return "IDLE";
  }
}

// Density in g/mL, or -1 when not valid (uncalibrated, or bypass still open).
float densityGml() {
  if (!DENSITY_CALIBRATED) return -1.0f;
  bool settled = (!bypassState && mainlineState) || !pumpState;   // same rule the LCD uses
  if (!settled) return -1.0f;
  float raw = densityPlaceholderGml;   // TODO (Oct 19): replace with the real measurement
  return calA * raw + calB;
}

byte statusFlags() {
  byte f = 0;
  if (pumpState)        f |= 1 << 0;
  if (bypassState)      f |= 1 << 1;
  if (mainlineState)    f |= 1 << 2;
  if (distanceTimedOut) f |= 1 << 3;
  if (sensorInitError)  f |= 1 << 4;
  return f;
}

void pulseCounter() {
  pulseCount++;
}

void setPump(bool on) {
  pumpState = on;
  digitalWrite(pumpPin, on ? HIGH : LOW);
}

void setBypass(bool open) {
  bypassState = open;
  digitalWrite(bypassPin, open ? HIGH : LOW);
}

void setMainline(bool open) {
  mainlineState = open;
  digitalWrite(mainlinePin, open ? HIGH : LOW);
}

// ---------------------------------------------------------------- protocol out
void sendTelemetry() {
  // $T,<phase>,<dens_gml>,<ml>,<g>,<target_g>,<dist_mm>,<flags>
  Serial.print(F("$T,"));
  Serial.print(phaseName());
  Serial.print(',');
  float d = densityGml();
  if (d < 0) Serial.print(-1); else Serial.print(d, 3);
  Serial.print(',');
  Serial.print(totalMilliLitres);
  Serial.print(',');
  Serial.print(totalGrams);
  Serial.print(',');
  Serial.print(targetGrams);
  Serial.print(',');
  Serial.print(rawDistance);
  Serial.print(',');
  Serial.println(statusFlags());
}

void sendEventStart() {
  Serial.print(F("$E,START,"));
  Serial.println(targetGrams);
}

void sendEventStop(const char* reason) {
  // $E,STOP,<ml>,<g>,<dens_gml>,<reason>
  Serial.print(F("$E,STOP,"));
  Serial.print(totalMilliLitres);
  Serial.print(',');
  Serial.print(totalGrams);
  Serial.print(',');
  float d = densityGml();
  if (d < 0) Serial.print(-1); else Serial.print(d, 3);
  Serial.print(',');
  Serial.println(reason);
}

void sendEventErr(const char* text) {
  Serial.print(F("$E,ERR,"));
  Serial.println(text);
}

// ---------------------------------------------------------------- fill control
void resetTotals() {
  noInterrupts();
  pulseCount = 0;
  interrupts();
  totalMilliLitres = 0;
  totalGrams = 0;
  fractionalMilliLitres = 0;
  lastFlowTime = millis();
}

void startSystem(unsigned long targetG) {
  targetGrams = targetG;
  resetTotals();

  setBypass(true);
  setMainline(false);
  setPump(true);

  phase = PRIMING;
  phaseStartedAt = millis();
  sendEventStart();
}

void stopSystem(const char* reason) {
  bool wasActive = (phase != IDLE) || pumpState || bypassState || mainlineState;
  phase = IDLE;
  setPump(false);
  setMainline(false);
  setBypass(false);
  if (wasActive) sendEventStop(reason);
}

// ---------------------------------------------------------------- commands in
void processCommand(char* line) {
  switch (line[0]) {
    case 'S':
      stopSystem("MANUAL");
      break;
    case 'P':   // manual toggles cancel the automatic sequence
      phase = IDLE;
      setPump(!pumpState);
      break;
    case 'B':
      phase = IDLE;
      setBypass(!bypassState);
      break;
    case 'M':
      phase = IDLE;
      setMainline(!mainlineState);
      break;
    case 'R': {
      if (phase != IDLE || pumpState) { sendEventErr("BUSY"); break; }
      float kg = atof(line + 1);
      if (kg < targetMinKg || kg > targetMaxKg) { sendEventErr("BAD_TARGET"); break; }
      startSystem((unsigned long)(kg * 1000.0f + 0.5f));
      break;
    }
    case 'C': {  // C<a>,<b>
      char* comma = strchr(line + 1, ',');
      if (!comma) { sendEventErr("BAD_CAL"); break; }
      *comma = '\0';
      calA = atof(line + 1);
      calB = atof(comma + 1);
      break;
    }
    default:
      break;   // ignore unknown bytes (e.g. bootloader chatter)
  }
}

void handleCommand() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    cmdLastByteAt = millis();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) { cmdBuf[cmdLen] = '\0'; processCommand(cmdBuf); cmdLen = 0; }
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    } else {
      cmdLen = 0;   // overlong, drop
    }
  }
  // Serial monitor set to "No line ending": finish the line after a short idle.
  if (cmdLen > 0 && millis() - cmdLastByteAt > cmdIdleMs) {
    cmdBuf[cmdLen] = '\0';
    processCommand(cmdBuf);
    cmdLen = 0;
  }
}

// ---------------------------------------------------------------- periodic
void updateSequence() {
  unsigned long now = millis();

  if (phase == PRIMING && now - phaseStartedAt >= primeTimeMs) {
    setMainline(true);
    phase = OVERLAP;
    phaseStartedAt = now;
  } else if (phase == OVERLAP && now - phaseStartedAt >= overlapTimeMs) {
    setBypass(false);
    phase = RUNNING;
  }

  if (phase != IDLE && totalGrams + stopMarginGrams >= targetGrams) {
    stopSystem("TARGET");
  }
}

void updateFlow() {
  unsigned long now = millis();
  unsigned long elapsedMs = now - lastFlowTime;
  if (elapsedMs < flowPeriodMs) return;

  // Read and reset the interrupt-updated counter together.
  noInterrupts();
  unsigned long pulses = pulseCount;
  pulseCount = 0;
  interrupts();

  lastFlowTime = now;

  float flowRateLpm = (1000.0f * pulses / elapsedMs) / calibrationFactor;

  // L/min x elapsed milliseconds / 60 = millilitres.
  float intervalMl = (flowRateLpm * elapsedMs / 60.0f) + fractionalMilliLitres;

  unsigned long wholeMl = (unsigned long)intervalMl;
  totalMilliLitres += wholeMl;
  fractionalMilliLitres = intervalMl - wholeMl;
  totalGrams = totalMilliLitres * densityPlaceholderGml;
}

void updateDistance() {
  if (sensorInitError) return;
  // Only read when a sample is ready, so this never blocks the loop.
  if (sensor.readReg(VL53L0X::RESULT_INTERRUPT_STATUS) & 0x07) {
    rawDistance = sensor.readRangeContinuousMillimeters();
    lastDistanceOkTime = millis();
    distanceTimedOut = false;
  } else if (millis() - lastDistanceOkTime > tofStaleMs) {
    distanceTimedOut = true;
  }
}

void updateDisplay() {
  unsigned long now = millis();
  if (now - lastDisplayTime < 250) return;
  lastDisplayTime = now;

  lcd.setCursor(0, 0);
  lcd.print("                ");
  lcd.setCursor(0, 0);

  lcd.print("D: ");
  if(!bypassState && mainlineState || !pumpState) {
    lcd.print(1.02);
  } else {
    lcd.print("----");
  }
  lcd.print("  L: ");
  lcd.print(totalMilliLitres*0.001, 2);

  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print("kg: ");
  if(!bypassState && mainlineState || !pumpState) {
    lcd.print(totalGrams*0.001, 2);
  } else {
    lcd.print("----");
  }
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  pinMode(pumpPin, OUTPUT);
  pinMode(bypassPin, OUTPUT);
  pinMode(mainlinePin, OUTPUT);
  digitalWrite(pumpPin, LOW);
  digitalWrite(bypassPin, LOW);
  digitalWrite(mainlinePin, LOW);

  pinMode(flowPin, INPUT_PULLUP);

  lcd.begin(16, 2);
  lcd.print("Starting sensor");

  Serial.begin(9600);
  Wire.begin();

  sensor.setTimeout(500);
  if (!sensor.init()) {
    sensorInitError = true;
    lcd.clear();
    lcd.print("Sensor error");
    delay(1500);               // DemoFinal halted here forever; v3 carries on without ToF
  } else {
    sensor.startContinuous();
  }

  lastFlowTime = millis();
  lastDistanceOkTime = millis();
  attachInterrupt(digitalPinToInterrupt(flowPin), pulseCounter, FALLING);
  lcd.clear();
  Serial.println(F("$E,BOOT"));
}

void loop() {
  handleCommand();
  updateSequence();
  updateFlow();
  updateDistance();
  updateDisplay();

  if (millis() - lastTelemetryTime >= telemetryPeriodMs) {
    lastTelemetryTime = millis();
    sendTelemetry();
  }
}
