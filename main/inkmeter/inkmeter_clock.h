#pragma once

#include <cstdint>
#include <functional>

namespace inkmeter {

// Clock ticks only queue work; networking and RTC I/O run on the Network worker.
void ServiceClock();
bool EnsureClock(const std::function<bool()> &still_valid = {},
                 int64_t deadline = 0);
bool ClockReady();
// Monotonic microseconds; zero when the system clock is usable.
int64_t ClockRetryAt();

} // namespace inkmeter
