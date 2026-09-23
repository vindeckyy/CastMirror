#ifndef CASTCORE_WIN_TIME_H_
#define CASTCORE_WIN_TIME_H_

#include <chrono>
#include <cstdint>

#if defined(_WIN32)

namespace castcore {

// Current raw QueryPerformanceCounter value.
uint64_t QpcNowTicks();

// Converts a raw QPC tick count into the std::chrono::steady_clock timeline.
//
// On Windows both the WGC/DXGI frame timestamps (LastPresentTime /
// SystemRelativeTime) and WASAPI's IAudioCaptureClient::GetBuffer QPC
// position are raw QPC values, while the encoders measure everything against
// std::chrono::steady_clock. steady_clock is QPC-backed on Windows, but its
// epoch is unspecified, so a constant offset is latched on first use. Both
// capture streams then share one timeline and A/V sync is exact.
std::chrono::steady_clock::time_point QpcTicksToSteadyClock(uint64_t qpc_ticks);

}  // namespace castcore

#endif  // _WIN32

#endif  // CASTCORE_WIN_TIME_H_
