// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/LiveRollbackOutputGate.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#include "Core/HW/EXI/EXI_Device.h"

#ifndef RINGOUT_ROLLBACK_GATE_TESTING
#include "Core/Core.h"
#include "Core/System.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#endif

namespace NetPlay
{
namespace
{
std::atomic<bool> s_hidden_replay_active{false};
std::atomic<bool> s_session_quarantine_active{false};
std::atomic<u64> s_audio_reset_generation{0};

constexpr std::size_t EffectIndex(const RollbackHostEffect effect)
{
  return static_cast<std::size_t>(effect);
}

bool BeginNoopCorrectedFrontierBarrier(bool* const gpu_was_running)
{
  *gpu_was_running = false;
  return true;
}

void EndNoopCorrectedFrontierBarrier(bool)
{
}

#ifndef RINGOUT_ROLLBACK_GATE_TESTING
bool BeginProductionCorrectedFrontierBarrier(bool* const gpu_was_running)
{
  // ViSwap work is queued to the video thread. Drain it while the hidden flag
  // is still armed, otherwise a replay swap queued before commit can observe
  // the cleared flag after commit and publish a stale frontier.
  AsyncRequests::GetInstance()->WaitForEmptyQueue();

  // Immediate-XFB presentation runs directly on the GPU thread instead of
  // the async ViSwap queue. Stop that thread at a FIFO-consistent point while
  // suppression remains armed, then resume it only after every replay
  // presentation path has observed the hidden epoch.
  Core::System& system = Core::System::GetInstance();
  Fifo::FifoManager& fifo = system.GetFifo();
  if (system.IsDualCoreMode() && !fifo.UseDeterministicGPUThread())
    return false;

  *gpu_was_running = Core::GetState(system) == Core::State::Running;
  fifo.PauseAndLock();
  return true;
}

void EndProductionCorrectedFrontierBarrier(const bool gpu_was_running)
{
  // EndHiddenReplay clears the hidden flag before calling this. Keeping the
  // FIFO paused across publication makes the transition atomic: all replay
  // FIFO work has crossed presentation while hidden, and no GPU work can
  // observe the visible epoch until this resume.
  Core::System::GetInstance().GetFifo().RestoreState(gpu_was_running);
}
#endif

struct CorrectedFrontierBarrier
{
  bool (*begin)(bool*);
  void (*end)(bool);
};

CorrectedFrontierBarrier GetCorrectedFrontierBarrier(const bool test_only)
{
  if (test_only)
    return {&BeginNoopCorrectedFrontierBarrier, &EndNoopCorrectedFrontierBarrier};
#ifndef RINGOUT_ROLLBACK_GATE_TESTING
  return {&BeginProductionCorrectedFrontierBarrier, &EndProductionCorrectedFrontierBarrier};
#else
  return {&BeginNoopCorrectedFrontierBarrier, &EndNoopCorrectedFrontierBarrier};
#endif
}
}  // namespace

std::string_view GetRollbackHostEffectName(const RollbackHostEffect effect)
{
  constexpr std::array<std::string_view, ROLLBACK_HOST_EFFECT_COUNT> names = {
      "video presentation",
      "frame dumping",
      "audio presentation",
      "buffered audio reconciliation",
      "controller rumble",
      "achievements",
      "movie recording",
      "persistent storage",
      "replay-derived netplay outbound traffic",
      "guest network traffic",
      "corrected frontier publication",
  };
  const std::size_t index = EffectIndex(effect);
  return index < names.size() ? names[index] : "unknown";
}

bool IsLiveRollbackHiddenReplayActive()
{
  return s_hidden_replay_active.load(std::memory_order_acquire);
}

bool IsLiveRollbackReplayDerivedOutboundAllowed()
{
  return !IsLiveRollbackHiddenReplayActive();
}

u64 GetLiveRollbackAudioResetGeneration()
{
  return s_audio_reset_generation.load(std::memory_order_acquire);
}

bool IsLiveRollbackSessionQuarantineActive()
{
  return s_session_quarantine_active.load(std::memory_order_acquire);
}

void FinalizeLiveRollbackOutputSuppressionAfterCoreTeardown()
{
  // A failed replay may leave the machine at an unpublished historical state.
  // Only the core teardown path, after every output producer has stopped, may
  // make global output visible again.
  s_hidden_replay_active.store(false, std::memory_order_release);
  s_session_quarantine_active.store(false, std::memory_order_release);
}

bool IsLiveRollbackProductionSessionPolicySafe(const LiveRollbackProductionSessionPolicy& policy)
{
  return policy.is_gamecube_title && !policy.save_data_writable && !policy.sd_writes_allowed &&
         policy.memory_card_slots_safe && policy.serial_port_1_disabled &&
         policy.serial_port_2_disabled && policy.gba_devices_disabled;
}

bool IsLiveRollbackMemoryCardSlotSafe(const ExpansionInterface::EXIDeviceType device)
{
  // Raw cards serialize their entire trusted on-disk size into every rollback
  // checkpoint. Folder cards serialize only the bounded active file state.
  return device == ExpansionInterface::EXIDeviceType::None ||
         device == ExpansionInterface::EXIDeviceType::MemoryCardFolder;
}

RollbackHostEffectCoverage LiveRollbackOutputGate::AuditedProductionCoverage()
{
  RollbackHostEffectCoverage coverage{};
  coverage[EffectIndex(RollbackHostEffect::VideoPresentation)] = true;
  coverage[EffectIndex(RollbackHostEffect::FrameDumping)] = true;
  coverage[EffectIndex(RollbackHostEffect::AudioPresentation)] = true;
  coverage[EffectIndex(RollbackHostEffect::BufferedAudioReconciliation)] = true;
  coverage[EffectIndex(RollbackHostEffect::ControllerRumble)] = true;
  coverage[EffectIndex(RollbackHostEffect::Achievements)] = true;
  coverage[EffectIndex(RollbackHostEffect::MovieRecording)] = true;
  coverage[EffectIndex(RollbackHostEffect::PersistentStorage)] = true;
  coverage[EffectIndex(RollbackHostEffect::ReplayDerivedNetPlayOutbound)] = true;
  coverage[EffectIndex(RollbackHostEffect::GuestNetwork)] = true;
  coverage[EffectIndex(RollbackHostEffect::CorrectedFrontierPublication)] = true;
  return coverage;
}

bool IsLiveRollbackProductionReady()
{
  const RollbackHostEffectCoverage coverage = LiveRollbackOutputGate::AuditedProductionCoverage();
  return std::ranges::all_of(coverage, [](const bool covered) { return covered; });
}

std::unique_ptr<LiveRollbackOutputGate>
LiveRollbackOutputGate::CreateProductionGate(const LiveRollbackProductionSessionPolicy& policy)
{
  auto gate = std::unique_ptr<LiveRollbackOutputGate>(new LiveRollbackOutputGate());
  if (!IsLiveRollbackProductionReady() || !IsLiveRollbackProductionSessionPolicySafe(policy))
    return nullptr;
  return gate;
}

std::unique_ptr<LiveRollbackOutputGate> LiveRollbackOutputGate::CreateHeadlessIsolatedTestGate()
{
  const char* const acknowledgement = std::getenv("RINGOUT_ROLLBACK_TEST_ACK");
  if (acknowledgement == nullptr || std::string_view(acknowledgement) != "HEADLESS_ISOLATED")
    return nullptr;

  RollbackHostEffectCoverage coverage{};
  coverage.fill(true);
  return std::unique_ptr<LiveRollbackOutputGate>(
      new LiveRollbackOutputGate(std::move(coverage), true));
}

#ifdef RINGOUT_ROLLBACK_GATE_TESTING
RollbackHostEffectCoverage LiveRollbackOutputGate::CompleteCoverageForTesting()
{
  RollbackHostEffectCoverage coverage{};
  coverage.fill(true);
  return coverage;
}

LiveRollbackOutputGate::LiveRollbackOutputGate(RollbackHostEffectCoverage coverage)
    : m_coverage(std::move(coverage))
{
  const CorrectedFrontierBarrier barrier = GetCorrectedFrontierBarrier(true);
  m_begin_corrected_frontier_barrier = barrier.begin;
  m_end_corrected_frontier_barrier = barrier.end;
}
#endif

LiveRollbackOutputGate::LiveRollbackOutputGate() : m_coverage(AuditedProductionCoverage())
{
  const CorrectedFrontierBarrier barrier = GetCorrectedFrontierBarrier(false);
  m_begin_corrected_frontier_barrier = barrier.begin;
  m_end_corrected_frontier_barrier = barrier.end;
}

LiveRollbackOutputGate::LiveRollbackOutputGate(RollbackHostEffectCoverage coverage,
                                               const bool test_only)
    : m_coverage(test_only ? std::move(coverage) : AuditedProductionCoverage())
{
  const CorrectedFrontierBarrier barrier = GetCorrectedFrontierBarrier(test_only);
  m_begin_corrected_frontier_barrier = barrier.begin;
  m_end_corrected_frontier_barrier = barrier.end;
}

LiveRollbackOutputGate::~LiveRollbackOutputGate()
{
  if (m_active)
    EndHiddenReplay(false);

  if (m_suppression_latched_until_teardown)
  {
    // Ownership transfers to the core teardown hook. Clearing either global
    // here could expose queued replay work while GPU/audio shutdown is pending.
    m_session_quarantine_active = false;
    return;
  }

  EndSessionQuarantine();
}

bool LiveRollbackOutputGate::BeginSessionQuarantine()
{
  if (m_session_quarantine_active || m_faulted)
    return false;

  bool expected = false;
  if (!s_session_quarantine_active.compare_exchange_strong(expected, true,
                                                           std::memory_order_acq_rel))
  {
    return false;
  }

  m_session_quarantine_active = true;
  return true;
}

void LiveRollbackOutputGate::EndSessionQuarantine()
{
  if (m_suppression_latched_until_teardown)
    return;
  if (!m_session_quarantine_active)
    return;
  m_session_quarantine_active = false;
  s_session_quarantine_active.store(false, std::memory_order_release);
}

bool LiveRollbackOutputGate::BeginHiddenReplay(const u64 first_frame,
                                               const u64 replay_through_frame)
{
  if (m_active || m_faulted || replay_through_frame < first_frame || !HasCompleteCoverage())
    return false;

  bool expected = false;
  if (!s_hidden_replay_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    return false;

  // The backend mixer consumes this epoch on its own thread, clears its
  // consumer queue there, and returns silence while hidden. Increment only
  // after this gate owns global suppression.
  if (s_audio_reset_generation.fetch_add(1, std::memory_order_acq_rel) ==
      std::numeric_limits<u64>::max())
  {
    s_hidden_replay_active.store(false, std::memory_order_release);
    m_faulted = true;
    return false;
  }

  m_active = true;
  return true;
}

bool LiveRollbackOutputGate::EndHiddenReplay(const bool corrected_frontier_is_publishable)
{
  if (!m_active)
  {
    m_faulted = true;
    return false;
  }

  bool publishable = corrected_frontier_is_publishable;
  bool gpu_was_running = false;
  bool frontier_paused = false;
  if (publishable)
  {
    if (!m_begin_corrected_frontier_barrier || !m_end_corrected_frontier_barrier)
    {
      publishable = false;
    }
    else
    {
      frontier_paused = m_begin_corrected_frontier_barrier(&gpu_was_running);
      publishable = frontier_paused;
    }
  }

  m_active = false;
  if (!publishable)
  {
    // Do not expose a historical/intermediate machine merely because the
    // session is about to stop. The teardown hook clears this latch only after
    // GPU, audio, and hardware output producers have ceased.
    m_faulted = true;
    m_suppression_latched_until_teardown = true;
    return false;
  }

  s_hidden_replay_active.store(false, std::memory_order_release);
  m_end_corrected_frontier_barrier(gpu_was_running);
  return true;
}

bool LiveRollbackOutputGate::HasCompleteCoverage() const
{
  return std::ranges::all_of(m_coverage, [](const bool covered) { return covered; });
}

}  // namespace NetPlay
