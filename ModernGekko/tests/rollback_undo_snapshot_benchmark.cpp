#include "Core/NetPlay/RollbackUndoSnapshotRing.h"

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
  constexpr std::size_t changed_bytes = 24'829;
  constexpr std::size_t iterations = 240;

  std::vector<std::uint8_t> memory(memory_size, 0x5a);
  std::vector<std::uint8_t> cpu_state(512, 0xa5);
  NetPlay::RollbackUndoSnapshotRing ring(memory_size, 10, changed_bytes, cpu_state.size());
  if (ring.GetConfigurationStatus() !=
      NetPlay::RollbackUndoSnapshotRing::ConfigurationStatus::Valid)
  {
    return 1;
  }

  std::vector<double> captures;
  std::vector<double> restores;
  captures.reserve(iterations);
  restores.reserve(iterations);
  for (std::size_t i = 0; i < iterations; ++i)
  {
    const std::uint64_t frame = i + 1;
    if (!ring.BeginFrame(frame, memory, cpu_state))
      return 1;
    const auto capture_start = Clock::now();
    for (std::size_t byte = 0; byte < changed_bytes; ++byte)
    {
      const std::size_t offset = (byte * 997 + i * 17) % memory_size;
      if (!ring.RecordWrite(offset, 1, memory))
        return 1;
      memory[offset] ^= static_cast<std::uint8_t>(byte + i + 1);
    }
    const auto capture_end = Clock::now();
    const auto restore_start = Clock::now();
    if (!ring.RestoreFrame(frame, memory, cpu_state))
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
  std::cout << "rollback_undo_snapshot_benchmark"
            << " unique_bytes=" << changed_bytes
            << " capture_p50_ms=" << percentile(captures, 50)
            << " capture_p95_ms=" << percentile(captures, 95)
            << " restore_p50_ms=" << percentile(restores, 50)
            << " restore_p95_ms=" << percentile(restores, 95) << '\n';
  return 0;
}
