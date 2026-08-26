#include "Core/NetPlay/RollbackRegionSnapshotRing.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

int main()
{
  using Clock = std::chrono::steady_clock;
  constexpr std::size_t MiB = 1024 * 1024;
  constexpr std::size_t memory_size = 24 * MiB;
  constexpr std::size_t profiled_bytes = 12 * MiB;
  constexpr std::size_t iterations = 240;

  std::vector<std::uint8_t> memory(memory_size, 0x5a);
  std::vector<std::uint8_t> cpu_state(512, 0xa5);
  NetPlay::RollbackRegionSnapshotRing ring(memory_size, {{0x3100, profiled_bytes}}, 10,
                                           cpu_state.size());
  if (ring.GetConfigurationStatus() !=
      NetPlay::RollbackRegionSnapshotRing::ConfigurationStatus::Valid)
  {
    return 1;
  }

  std::vector<double> captures;
  std::vector<double> restores;
  captures.reserve(iterations);
  restores.reserve(iterations);
  for (std::size_t i = 0; i < iterations; ++i)
  {
    memory[0x3100 + (i % profiled_bytes)] ^= static_cast<std::uint8_t>(i);
    const auto capture_start = Clock::now();
    if (!ring.Capture(i, memory, cpu_state))
      return 1;
    const auto capture_end = Clock::now();
    memory[0x3100] ^= 1;
    const auto restore_start = Clock::now();
    if (!ring.Restore(i, memory, cpu_state))
      return 1;
    const auto restore_end = Clock::now();
    captures.push_back(
        std::chrono::duration<double, std::milli>(capture_end - capture_start).count());
    restores.push_back(
        std::chrono::duration<double, std::milli>(restore_end - restore_start).count());
  }

  const auto percentile = [](std::vector<double> samples, const std::size_t numerator) {
    std::sort(samples.begin(), samples.end());
    const std::size_t index = std::min(samples.size() - 1, samples.size() * numerator / 100);
    return samples[index];
  };
  std::cout << "rollback_region_snapshot_benchmark"
            << " bytes=" << ring.GetSnapshotSize() << " capture_p50_ms=" << percentile(captures, 50)
            << " capture_p95_ms=" << percentile(captures, 95)
            << " restore_p50_ms=" << percentile(restores, 50)
            << " restore_p95_ms=" << percentile(restores, 95) << '\n';
  return 0;
}
