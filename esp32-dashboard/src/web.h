#pragma once
#include <Arduino.h>
#include "store.h"

namespace Web {
  void begin();
  void loop();
  // Called by main when a $E,STOP has been written to the CSV; /live shows a
  // "Flight N saved" notice with a link to the slip for a short while.
  void noteSessionSaved(uint32_t id);
  // Flight details typed in before the last START (attached to the session on STOP).
  const FlightInfo& currentFlight();
}
