// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/LiveRollbackOutputGate.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>

namespace NetPlay
{
namespace
{
std::atomic<bool> s_hidden_replay_active{false};

constexpr std::size_t EffectIndex(const RollbackHostEffect effect)
{
  return static_cast<std::size_t>(effect);
}
}  // namespace

std::string_view GetRollbackHostEffectName(const RollbackHostEffect effect)
{
  constexpr std::array<std::string_view, ROLLBACK_HOST_EFFECT_COUNT> names = {
      "video presentation",
      "frame dumping",
      "audio presentation",
      "audio reconciliation",
      "controller rumble",
      "achievements",
      "movie recording",
      "persistent storage",
      "netplay outbound traffic",
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

RollbackHostEffectCoverage LiveRollbackOutputGate::AuditedProductionCoverage()
{
  RollbackHostEffectCoverage coverage{};
  coverage[EffectIndex(RollbackHostEffect::VideoPresentation)] = true;
  return coverage;
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
}
#endif

LiveRollbackOutputGate::LiveRollbackOutputGate() : m_coverage(AuditedProductionCoverage())
{
}

LiveRollbackOutputGate::LiveRollbackOutputGate(RollbackHostEffectCoverage coverage,
                                               const bool test_only)
    : m_coverage(test_only ? std::move(coverage) : AuditedProductionCoverage())
{
}

LiveRollbackOutputGate::~LiveRollbackOutputGate()
{
  if (m_active)
  {
    s_hidden_replay_active.store(false, std::memory_order_release);
    m_active = false;
  }
}

bool LiveRollbackOutputGate::BeginHiddenReplay(const u64 first_frame,
                                               const u64 replay_through_frame)
{
  if (m_active || m_faulted || replay_through_frame < first_frame || !HasCompleteCoverage())
    return false;

  bool expected = false;
  if (!s_hidden_replay_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    return false;

  m_active = true;
  return true;
}

void LiveRollbackOutputGate::EndHiddenReplay(const bool corrected_frontier_is_publishable)
{
  if (!m_active)
  {
    m_faulted = true;
    return;
  }

  m_active = false;
  s_hidden_replay_active.store(false, std::memory_order_release);
  if (!corrected_frontier_is_publishable)
    m_faulted = true;
}

bool LiveRollbackOutputGate::HasCompleteCoverage() const
{
  return std::ranges::all_of(m_coverage, [](const bool covered) { return covered; });
}

}  // namespace NetPlay
