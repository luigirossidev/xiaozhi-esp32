#pragma once

#include "clock_face.h"

// Peppe: Rome time for the clock screen.
//
// The time comes from SNTP (pool.ntp.org). It is kept as an offset from the
// monotonic esp_timer, so it stays right even when other code sets the system
// clock: the OTA check sets it to the server's time zone, not Rome's. Daylight
// saving time follows the EU rule (last Sunday of March and of October, 01:00
// UTC), so no time zone database is needed.
namespace rome_time {

// Starts SNTP; safe to call more than once. Call it once the network is up.
void StartSync();

// Fills time with the current Rome time; time.valid is false until the first
// successful synchronisation.
void Now(clock_face::Time& time);

}  // namespace rome_time
