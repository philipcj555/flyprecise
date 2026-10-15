#pragma once
#include <Arduino.h>

namespace Web {
  void begin();
  void loop();
  // Called by main when a $E,STOP has been written to the CSV; /live shows a
  // "Session N saved" notice with a link to the slip for a short while.
  void noteSessionSaved(uint32_t id);
}
