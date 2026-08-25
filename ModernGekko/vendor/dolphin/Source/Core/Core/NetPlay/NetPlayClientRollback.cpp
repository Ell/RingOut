// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/NetPlayClientRollback.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>

#include "Core/Core.h"
#include "Core/System.h"

namespace NetPlay
{
namespace
{
constexpr u32 MAX_ROLLBACK_POLLS_PER_FRAME = 8;
constexpr std::size_t ROLLBACK_REDUNDANT_BATCHES = 3;

u8 PadBit(const std::size_t pad)
{
  return static_cast<u8>(u8{1} << pad);
}

bool IsHeadlessIsolatedTestAcknowledged()
{
  const char* const acknowledgement = std::getenv("RINGOUT_ROLLBACK_TEST_ACK");
  return acknowledgement != nullptr && std::string_view(acknowledgement) == "HEADLESS_ISOLATED";
}
}  // namespace

bool NetPlayClient::UpdateLiveRollbackFrameBoundaryImpl()
{
  if (!Core::IsCPUThread())
    return false;
  if (m_live_rollback && m_rollback_protocol_fault.IsSet())
  {
    FaultLiveRollbackImpl();
    return false;
  }
  const RollbackNetplaySession session = GetRollbackNetplaySession();
  if (!session.enabled)
  {
    ResetLiveRollbackImpl();
    return true;
  }
  if (!m_is_running.IsSet())
  {
    ResetLiveRollbackImpl();
    return true;
  }

  if (!m_live_rollback)
  {
    auto live = std::make_unique<LiveRollbackState>();
    live->session = session;

    if (!IsHeadlessIsolatedTestAcknowledged() || session.base_delay_samples == 0 ||
        session.rollback_horizon_frames >
            std::numeric_limits<u64>::max() / MAX_ROLLBACK_POLLS_PER_FRAME)
    {
      std::fprintf(stderr,
                   "[rollback live] activation refused: isolated output-safety acknowledgement "
                   "or numeric contract missing\n");
      return false;
    }

    std::array<RollbackInputTimeline::PadAuthority, RollbackInputTimeline::PAD_COUNT> authority{};
    for (std::size_t pad = 0; pad < authority.size(); ++pad)
    {
      if (m_pad_map[pad] <= 0)
        authority[pad] = RollbackInputTimeline::PadAuthority::Inactive;
      else if (IsLocalPlayer(m_pad_map[pad]))
        authority[pad] = RollbackInputTimeline::PadAuthority::Local;
      else
        authority[pad] = RollbackInputTimeline::PadAuthority::Remote;
    }

    // A batch-distance bound must never imply a larger frame-distance bound.
    // There is at least one grouped SI update in each emulated frame, so one
    // predicted batch per negotiated horizon frame is conservative under
    // variable (usually two-per-frame) polling. Multiplying by the maximum poll
    // count would let a one-poll frame outrun the snapshot ring.
    const u64 prediction_batches = session.rollback_horizon_frames;
    const u64 required_history = static_cast<u64>(session.base_delay_samples) + prediction_batches +
                                 MAX_ROLLBACK_POLLS_PER_FRAME;
    if (required_history > std::numeric_limits<std::size_t>::max())
      return false;

    live->state_store = std::make_unique<DolphinRollbackStateStore>(
        Core::System::GetInstance(), static_cast<std::size_t>(session.rollback_horizon_frames) + 2);
    live->output_gate = LiveRollbackOutputGate::CreateHeadlessIsolatedTestGate();
    if (!live->output_gate || live->state_store->GetConfigurationStatus() !=
                                  DolphinRollbackStateStore::ConfigurationStatus::Valid)
    {
      std::fprintf(stderr, "[rollback live] activation refused: state/output gate unavailable\n");
      return false;
    }

    live->scheduler =
        std::make_unique<LiveRollbackInputScheduler>(LiveRollbackInputScheduler::Config{
            .protocol_version = session.protocol_version,
            .session_generation = session.generation,
            .first_batch_id = 0,
            .history_capacity = static_cast<std::size_t>(required_history),
            .base_delay_batches = session.base_delay_samples,
            .max_prediction_batches = prediction_batches,
            .max_polls_per_frame = MAX_ROLLBACK_POLLS_PER_FRAME,
            .redundant_batch_count = ROLLBACK_REDUNDANT_BATCHES,
            .pad_authority = authority});
    if (live->scheduler->GetConfigurationStatus() !=
        LiveRollbackInputScheduler::ConfigurationStatus::Valid)
    {
      std::fprintf(stderr, "[rollback live] activation refused: SI scheduler contract invalid\n");
      return false;
    }

    live->coordinator = std::make_unique<RollbackCoordinator>(
        RollbackCoordinator::Config{
            .enabled = true,
            .max_replay_frames = static_cast<std::size_t>(session.rollback_horizon_frames) + 1},
        *live->state_store, *live->output_gate);
    live->frame_boundary = std::make_unique<LiveRollbackFrameBoundary>(
        *live->coordinator, live->scheduler->GetJournal(), *live->output_gate);

    // This hook runs at the end of physical frame zero, so the state captured
    // here is exactly the start of logical frame one.
    if (live->frame_boundary->Activate(1) != LiveRollbackFrameBoundary::ActivationStatus::Active)
    {
      std::fprintf(stderr, "[rollback live] activation refused: initial checkpoint failed\n");
      return false;
    }

    live->active = true;
    m_live_rollback = std::move(live);
    std::fprintf(stderr,
                 "[rollback live] active generation=%llu snapshot_frames=%u "
                 "prediction_batches=%llu\n",
                 static_cast<unsigned long long>(session.generation),
                 session.rollback_horizon_frames + 2,
                 static_cast<unsigned long long>(prediction_batches));
    std::fflush(stderr);
    return true;
  }

  LiveRollbackState& live = *m_live_rollback;
  if (!live.active || live.faulted || live.session.generation != session.generation)
  {
    FaultLiveRollbackImpl();
    return false;
  }

  // A correction may arrive after the final SI poll but before the frame
  // boundary. Drain it before the boundary decides whether to restore.
  RollbackSIInputPacket incoming;
  while (TryPopRollbackSIInput(&incoming))
  {
    const auto status = live.scheduler->SubmitRemotePacket(incoming);
    if (status == LiveRollbackInputScheduler::RemotePacketStatus::CorrectedPrediction)
    {
      std::fprintf(stderr, "[rollback live] correction received before frame boundary\n");
    }
    else if (status != LiveRollbackInputScheduler::RemotePacketStatus::Accepted &&
             status != LiveRollbackInputScheduler::RemotePacketStatus::DuplicateOrRetired)
    {
      FaultLiveRollbackImpl();
      return false;
    }
  }

  const auto status = live.frame_boundary->CompleteCurrentFrame();
  switch (status)
  {
  case LiveRollbackFrameBoundary::BoundaryStatus::Advanced:
  case LiveRollbackFrameBoundary::BoundaryStatus::ReplayAdvanced:
    return true;
  case LiveRollbackFrameBoundary::BoundaryStatus::RollbackStarted:
  case LiveRollbackFrameBoundary::BoundaryStatus::ChainedRollbackStarted:
  {
    if (status == LiveRollbackFrameBoundary::BoundaryStatus::ChainedRollbackStarted)
      live.scheduler->CancelReplay();
    const auto trigger = live.scheduler->GetReplayTrigger();
    if (!trigger || !live.scheduler->StartReplay(*trigger))
    {
      FaultLiveRollbackImpl();
      return false;
    }
    live.last_poll_frame.reset();
    live.next_poll_ordinal = 0;
    std::fprintf(stderr,
                 "[rollback live] correction restore_frame=%llu replay_through_frame=%llu "
                 "first_batch=%llu\n",
                 static_cast<unsigned long long>(trigger->restore_before_emulated_frame),
                 static_cast<unsigned long long>(trigger->replay_through_emulated_frame),
                 static_cast<unsigned long long>(trigger->first_replay_batch_id));
    std::fflush(stderr);
    return true;
  }
  case LiveRollbackFrameBoundary::BoundaryStatus::ReplayCommitted:
    live.scheduler->CancelReplay();
    live.last_poll_frame.reset();
    live.next_poll_ordinal = 0;
    std::fprintf(stderr, "[rollback live] correction committed\n");
    std::fflush(stderr);
    return true;
  case LiveRollbackFrameBoundary::BoundaryStatus::Inactive:
  case LiveRollbackFrameBoundary::BoundaryStatus::Faulted:
    FaultLiveRollbackImpl();
    return false;
  }
  FaultLiveRollbackImpl();
  return false;
}

bool NetPlayClient::IsLiveRollbackSessionActiveImpl() const
{
  return (m_live_rollback && m_live_rollback->active) || GetRollbackNetplaySession().enabled;
}

bool NetPlayClient::GetLiveRollbackPads(const int pad_nb, const bool batching,
                                        GCPadStatus* const pad_status, bool* const handled)
{
  *handled = false;
  if (m_live_rollback && m_rollback_protocol_fault.IsSet())
  {
    *handled = true;
    FaultLiveRollbackImpl();
    return false;
  }
  const RollbackNetplaySession session = GetRollbackNetplaySession();
  if (!session.enabled || !m_live_rollback)
    return false;

  LiveRollbackState& live = *m_live_rollback;
  *handled = true;
  if (!Core::IsCPUThread() || !live.active || live.faulted || pad_status == nullptr || pad_nb < 0 ||
      pad_nb >= static_cast<int>(RollbackInputTimeline::PAD_COUNT))
  {
    FaultLiveRollbackImpl();
    return false;
  }

  // Grouped UpdateDevices polls resolve once at the first mapped pad. Guest SI
  // transfers occur outside that grouping and are each independent journaled
  // polls, while still resolving the full active-pad set deterministically.
  if (!batching || IsFirstInGamePad(pad_nb))
  {
    const std::optional<u64> frame = live.frame_boundary->GetCurrentFrame();
    if (!frame)
    {
      FaultLiveRollbackImpl();
      return false;
    }

    u32 ordinal = 0;
    if (live.last_poll_frame && *live.last_poll_frame == *frame)
    {
      if (live.next_poll_ordinal == std::numeric_limits<u32>::max())
      {
        FaultLiveRollbackImpl();
        return false;
      }
      ordinal = ++live.next_poll_ordinal;
    }
    else
    {
      live.last_poll_frame = *frame;
      live.next_poll_ordinal = 0;
    }

    const u8 local_mask = live.scheduler->GetLocalPadMask();
    std::array<GCPadStatus, RollbackInputTimeline::PAD_COUNT> sampled{};
    if (!live.scheduler->IsReplaying())
    {
      for (std::size_t pad = 0; pad < sampled.size(); ++pad)
      {
        if ((local_mask & PadBit(pad)) == 0)
          continue;
        const int local_pad = InGamePadToLocalPad(static_cast<int>(pad));
        if (local_pad < 0 || local_pad >= 4)
        {
          FaultLiveRollbackImpl();
          return false;
        }
        sampled[pad] = SampleLocalPad(local_pad);
      }
    }

    LiveRollbackInputScheduler::BatchResult result;
    while (m_is_running.IsSet())
    {
      RollbackSIInputPacket incoming;
      while (TryPopRollbackSIInput(&incoming))
      {
        const auto remote = live.scheduler->SubmitRemotePacket(incoming);
        if (remote != LiveRollbackInputScheduler::RemotePacketStatus::Accepted &&
            remote != LiveRollbackInputScheduler::RemotePacketStatus::CorrectedPrediction &&
            remote != LiveRollbackInputScheduler::RemotePacketStatus::DuplicateOrRetired)
        {
          FaultLiveRollbackImpl();
          return false;
        }
      }

      if (live.scheduler->IsReplaying())
      {
        result = live.scheduler->BeginReplayBatch(*frame, ordinal);
      }
      else
      {
        result = live.scheduler->BeginNormalBatch(*frame, ordinal, local_mask, sampled);
        if (result.outgoing_packet)
        {
          const auto& packet = *result.outgoing_packet;
          const u64 newest = packet.batches[packet.batch_count - 1].batch_id;
          if (!live.last_sent_future_batch || *live.last_sent_future_batch != newest)
          {
            if (!SendRollbackSIInput(packet))
            {
              FaultLiveRollbackImpl();
              return false;
            }
            live.last_sent_future_batch = newest;
          }
        }
      }

      if (result.status == LiveRollbackInputScheduler::BatchStatus::Resolved)
      {
        if (live.horizon_wait_logged)
        {
          std::fprintf(stderr, "[rollback live] horizon resumed with authoritative input\n");
          std::fflush(stderr);
        }
        live.horizon_wait_logged = false;
        break;
      }
      if (result.status != LiveRollbackInputScheduler::BatchStatus::AwaitingRemoteInput)
      {
        FaultLiveRollbackImpl();
        return false;
      }

      if (!live.horizon_wait_logged)
      {
        std::fprintf(stderr, "[rollback live] horizon fallback: waiting for authoritative input\n");
        std::fflush(stderr);
        live.horizon_wait_logged = true;
      }
      WaitForRollbackSIInput(std::chrono::milliseconds(100));
      // A hard horizon freezes guest execution, not the network protocol. Keep
      // retransmitting the same bounded redundant tail so symmetric stalls and
      // deterministic fault injection cannot deadlock delivery forever.
      if (result.outgoing_packet && !SendRollbackSIInput(*result.outgoing_packet))
      {
        FaultLiveRollbackImpl();
        return false;
      }
    }

    if (!m_is_running.IsSet())
    {
      DeactivateLiveRollbackImpl();
      return false;
    }
    live.current_inputs = result.inputs;
  }

  if (!live.current_inputs ||
      (live.current_inputs->active_pad_mask & PadBit(static_cast<std::size_t>(pad_nb))) == 0)
  {
    FaultLiveRollbackImpl();
    return false;
  }
  *pad_status = live.current_inputs->pads[pad_nb];
  return true;
}

void NetPlayClient::DeactivateLiveRollbackImpl()
{
  if (!m_live_rollback)
    return;

  // Both CPU entry points are serialized by crit_netplay_client. Stop and
  // destruction may call this from their owner thread while holding that same
  // lock, so no CPU callback can retain a reference into this state here.
  if (m_live_rollback->frame_boundary)
    m_live_rollback->frame_boundary->Deactivate();
  if (m_live_rollback->scheduler)
    m_live_rollback->scheduler->CancelReplay();
  m_live_rollback->active = false;
}

void NetPlayClient::FaultLiveRollbackImpl()
{
  if (!m_live_rollback)
    return;
  m_live_rollback->faulted = true;
  DeactivateLiveRollbackImpl();
}

void NetPlayClient::ResetLiveRollbackImpl()
{
  DeactivateLiveRollbackImpl();
  m_live_rollback.reset();
}

}  // namespace NetPlay
