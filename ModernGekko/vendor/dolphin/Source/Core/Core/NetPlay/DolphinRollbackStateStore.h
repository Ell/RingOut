// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"
#include "Core/NetPlay/RollbackCoordinator.h"

namespace Core
{
class System;
}

namespace NetPlay
{

// Production full-emulator checkpoint storage. All methods must run on the CPU
// thread at a stable frame boundary. Slots are reused by frame modulo capacity;
// a correction older than the ring is rejected by HasFrameStart.
class DolphinRollbackStateStore final : public RollbackStateStore
{
public:
  enum class ConfigurationStatus : u8
  {
    Valid,
    ZeroCapacity,
    ExperimentalSnapshotSkipActive,
  };

  DolphinRollbackStateStore(Core::System& system, std::size_t capacity);

  bool CaptureFrameStart(u64 frame) override;
  bool HasFrameStart(u64 frame) const override;
  bool RestoreFrameStart(u64 frame) override;

  ConfigurationStatus GetConfigurationStatus() const { return m_configuration_status; }
  std::size_t GetCapacity() const { return m_slots.size(); }
  std::optional<std::size_t> GetSnapshotSize(u64 frame) const;

private:
  struct Slot
  {
    Common::UniqueBuffer<u8> buffer;
    std::size_t valid_size = 0;
    std::optional<u64> frame;
  };

  Slot* FindSlot(u64 frame);
  const Slot* FindSlot(u64 frame) const;

  Core::System& m_system;
  std::vector<Slot> m_slots;
  const ConfigurationStatus m_configuration_status;
  std::optional<std::size_t> m_last_reported_snapshot_size;
};

}  // namespace NetPlay
