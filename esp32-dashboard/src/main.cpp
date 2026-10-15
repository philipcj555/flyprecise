// FlyPrecise ESP32 dashboard - entry point.
// The ESP32 is a Wi-Fi front end only. The Uno owns sensors, valves, pump and LCD.
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "config.h"
#include "uno_link.h"
#include "store.h"
#include "web.h"

static DNSServer dns;

static void onUnoStop(const StopEvent& ev) {
  uint32_t id = Store::appendSession(ev, Web::currentFlight());
  Web::noteSessionSaved(id);
}

void setup() {
  Serial.begin(115200);           // native USB CDC, debug only
  delay(200);
  Serial.println();
  Serial.printf("FlyPrecise ESP32 dashboard  (FAKE_UNO=%d)\n", FAKE_UNO);

  Store::begin();

  const char* pass = Store::settings().apPass[0] ? Store::settings().apPass : AP_PASS_DEFAULT;
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
  WiFi.softAP(AP_SSID, pass);
  Serial.printf("AP \"%s\" up at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  // Captive portal: answer every DNS query with our IP.
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", AP_IP);
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);

  UnoLink::onStop(onUnoStop);
  UnoLink::begin();
  Web::begin();
  Serial.println("ready");
}

void loop() {
  dns.processNextRequest();
  Web::loop();
  UnoLink::poll();
}
