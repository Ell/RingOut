// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
  std::optional<u64> current_input_batch;
  std::optional<u64> last_sent_future_batch;
  std::optional<u64> last_poll_frame;
  std::optional<RollbackSIInputJournal::ReplayTrigger> sc2_selective_trigger;
  std::optional<u32> digest_fault_frame;
  RollbackStateDigestCandidates digest_candidates;
  std::FILE* confirmed_state_log = nullptr;
  std::string confirmed_state_dump_path;
  std::optional<u32> confirmed_state_dump_frame;
  std::string mismatch_state_dump_path;
  std::map<u32, std::vector<u8>> mismatch_state_dump_candidates;
  u32 next_poll_ordinal = 0;
  bool horizon_wait_logged = false;
  bool digest_fault_applied = false;
  bool sc2_selective_enabled = false;
  bool sc2_selective_rearm_after_broad = false;
  bool active = false;
  bool faulted = false;

  ~LiveRollbackState()
  {
    if (confirmed_state_log)
      std::fclose(confirmed_state_log);
  }
};

}  // namespace NetPlay
