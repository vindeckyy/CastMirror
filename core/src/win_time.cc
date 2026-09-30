#include "castcore/win_time.h"

#if defined(_WIN32)

#include <windows.h>

#include <mutex>

namespace castcore {

namespace {

std::once_flag g_init_once;
int64_t g_qpc_origin_ticks = 0;
int64_t g_steady_origin_ns = 0;
int64_t g_qpc_freq = 1;

void InitOnce() {
  LARGE_INTEGER freq{};
  LARGE_INTEGER now{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  g_qpc_freq = freq.QuadPart > 0 ? freq.QuadPart : 1;
  g_qpc_origin_ticks = now.QuadPart;
  g_steady_origin_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
}

}  // namespace

uint64_t QpcNowTicks() {
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return static_cast<uint64_t>(now.QuadPart);
}

std::chrono::steady_clock::time_point QpcTicksToSteadyClock(uint64_t qpc_ticks) {
  std::call_once(g_init_once, InitOnce);

  const int64_t delta_ticks = static_cast<int64_t>(qpc_ticks) - g_qpc_origin_ticks;
  const __int128 delta_ns = static_cast<__int128>(delta_ticks) * 1000000000LL / g_qpc_freq;
  const int64_t total_ns = g_steady_origin_ns + static_cast<int64_t>(delta_ns);

  return std::chrono::steady_clock::time_point(
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::nanoseconds(total_ns)));
}

}  // namespace castcore

#endif  // _WIN32
