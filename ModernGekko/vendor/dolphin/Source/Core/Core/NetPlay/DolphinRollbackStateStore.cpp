// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/DolphinRollbackStateStore.h"

#include <cstdio>
#include <span>

#include "Core/Core.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/Fifo.h"

namespace NetPlay
{
namespace
{
// State::DoState serializes video first on the GPU thread, then CoreTiming,
// hardware, memory, and PowerPC state on the CPU thread.  A blocking video
// request makes the video section internally coherent, but without this guard
// the GPU may resume between those sections.  That creates a torn checkpoint
// (or load) whose FIFO parser and guest FIFO memory come from different
// timelines.  Keep emulated GPU execution stopped for the complete operation;
// AsyncRequests still run while paused and explicitly wake the host GPU thread.
class ScopedRollbackGpuQuiescence final
{
public:
  explicit ScopedRollbackGpuQuiescence(Core::System& system)
      : m_fifo(system.GetFifo()), m_was_running(Core::GetState(system) == Core::State::Running)
  {
    m_fifo.PauseAndLock();
  }

  ~ScopedRollbackGpuQuiescence() { m_fifo.RestoreState(m_was_running); }

  ScopedRollbackGpuQuiescence(const ScopedRollbackGpuQuiescence&) = delete;
  ScopedRollbackGpuQuiescence& operator=(const ScopedRollbackGpuQuiescence&) = delete;

private:
  Fifo::FifoManager& m_fifo;
  bool m_was_running;
};
}  // namespace

DolphinRollbackStateStore::DolphinRollbackStateStore(Core::System& system,
                                                     const std::size_t capacity)
    : m_system(system), m_slots(capacity),
      m_configuration_status(capacity == 0 ? ConfigurationStatus::ZeroCapacity :
                             State::SnapshotSkipMask() != State::SKIP_NONE ?
                                             ConfigurationStatus::ExperimentalSnapshotSkipActive :
                                             ConfigurationStatus::Valid)
{
}

bool DolphinRollbackStateStore::CaptureFrameStart(const u64 frame)
{
  if (m_configuration_status != ConfigurationStatus::Valid || !Core::IsCPUThread())
    return false;

  Slot& slot = m_slots[frame % m_slots.size()];
  // Invalidate first: a failed overwrite must never leave stale bytes labeled
  // as either the old frame or the requested new frame.
  slot.frame.reset();
  slot.valid_size = 0;
  const ScopedRollbackGpuQuiescence gpu_quiescence(m_system);
  const State::ScopedRollbackSnapshot rollback_snapshot_scope;
  const std::size_t size = State::SaveToBuffer(m_system, slot.buffer);
  if (size == 0)
    return false;
  slot.valid_size = size;
  slot.frame = frame;
  if (!m_last_reported_snapshot_size.has_value())
  {
    std::fprintf(stderr,
                 "[rollback state] checkpoint frame=%llu bytes=%zu (%.2f MiB); "
                 "addressable MEM1 + lossless zero-aware VMEM; "
                 "gpu_transaction_barrier=%s\n",
                 static_cast<unsigned long long>(frame), size,
                 static_cast<double>(size) / (1024.0 * 1024.0),
                 m_system.IsDualCoreMode() ? "dual-core-quiesced" : "single-core-quiesced");
    std::fflush(stderr);
    m_last_reported_snapshot_size = size;
  }
  return true;
}

bool DolphinRollbackStateStore::HasFrameStart(const u64 frame) const
{
  return Core::IsCPUThread() && FindSlot(frame) != nullptr;
}

bool DolphinRollbackStateStore::RestoreFrameStart(const u64 frame)
{
  if (!Core::IsCPUThread())
    return false;
  Slot* const slot = FindSlot(frame);
  if (slot == nullptr)
    return false;
  const ScopedRollbackGpuQuiescence gpu_quiescence(m_system);
  const State::ScopedRollbackSnapshot rollback_snapshot_scope;
  return State::LoadFromBuffer(m_system, std::span<u8>{slot->buffer.data(), slot->valid_size});
}

std::optional<std::size_t> DolphinRollbackStateStore::GetSnapshotSize(const u64 frame) const
{
  if (!Core::IsCPUThread())
    return std::nullopt;
  const Slot* const slot = FindSlot(frame);
  if (slot == nullptr)
    return std::nullopt;
  return slot->valid_size;
}

DolphinRollbackStateStore::Slot* DolphinRollbackStateStore::FindSlot(const u64 frame)
{
  if (m_configuration_status != ConfigurationStatus::Valid)
    return nullptr;
  Slot& slot = m_slots[frame % m_slots.size()];
  return slot.frame == frame ? &slot : nullptr;
}

const DolphinRollbackStateStore::Slot* DolphinRollbackStateStore::FindSlot(const u64 frame) const
{
  if (m_configuration_status != ConfigurationStatus::Valid)
    return nullptr;
  const Slot& slot = m_slots[frame % m_slots.size()];
  return slot.frame == frame ? &slot : nullptr;
}

}  // namespace NetPlay
