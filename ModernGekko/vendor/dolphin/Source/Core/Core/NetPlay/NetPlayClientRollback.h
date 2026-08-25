// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>

#include "Core/NetPlay/DolphinRollbackStateStore.h"
#include "Core/NetPlay/LiveRollbackFrameBoundary.h"
#include "Core/NetPlay/LiveRollbackInputScheduler.h"
#include "Core/NetPlay/LiveRollbackOutputGate.h"
#include "Core/NetPlay/NetPlayClient.h"

namespace NetPlay
{

// Private CPU-thread-owned state kept out of the public NetPlayClient header.
// The network thread communicates with it only through the bounded SPSC RSIB
// queue already owned by NetPlayClient.
struct NetPlayClient::LiveRollbackState
{
  RollbackNetplaySession session{};
  std::unique_ptr<DolphinRollbackStateStore> state_store;
  std::unique_ptr<LiveRollbackOutputGate> output_gate;
  std::unique_ptr<RollbackCoordinator> coordinator;
  std::unique_ptr<LiveRollbackInputScheduler> scheduler;
  std::unique_ptr<LiveRollbackFrameBoundary> frame_boundary;

  std::optional<RollbackInputTimeline::ResolvedFrame> current_inputs;
  std::optional<u64> last_sent_future_batch;
  std::optional<u64> last_poll_frame;
  u32 next_poll_ordinal = 0;
  bool horizon_wait_logged = false;
  bool active = false;
  bool faulted = false;
};

}  // namespace NetPlay
