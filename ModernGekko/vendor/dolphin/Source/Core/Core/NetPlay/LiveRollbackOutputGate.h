// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/NetPlay/RollbackCoordinator.h"

namespace NetPlay
{

// Host-visible effects which must be suppressed, reconciled, or proved absent
// before an emulator state may move backwards. Keep this list explicit: adding
// a new effect defaults it to uncovered and therefore prevents activation.
enum class RollbackHostEffect : u8
{
  VideoPresentation,
  FrameDumping,
  AudioPresentation,
  AudioReconciliation,
  ControllerRumble,
  Achievements,
  MovieRecording,
  PersistentStorage,
  NetPlayOutbound,
  GuestNetwork,
  CorrectedFrontierPublication,
  Count,
};

constexpr std::size_t ROLLBACK_HOST_EFFECT_COUNT =
    static_cast<std::size_t>(RollbackHostEffect::Count);
using RollbackHostEffectCoverage = std::array<bool, ROLLBACK_HOST_EFFECT_COUNT>;

std::string_view GetRollbackHostEffectName(RollbackHostEffect effect);

// Global read-only query used at narrow host-output choke points. The flag is
// published only after the complete capability matrix has passed.
bool IsLiveRollbackHiddenReplayActive();

class LiveRollbackOutputGate final : public RollbackOutputGate
{
public:
  // Coverage implemented in the current RingOut runtime. This intentionally
  // remains incomplete: audio reconciliation, persistent writes, replay-time
  // input transmission, guest networking, and atomic corrected-frontier
  // publication are not yet safe.
  static RollbackHostEffectCoverage AuditedProductionCoverage();

  // Explicit escape hatch for the isolated two-process proof harness. It is
  // available only when RINGOUT_ROLLBACK_TEST_ACK=HEADLESS_ISOLATED, never from
  // ordinary lobby settings. The harness accepts that speculative host effects
  // are disposable so it can exercise real restore/resimulation end to end.
  static std::unique_ptr<LiveRollbackOutputGate> CreateHeadlessIsolatedTestGate();

#ifdef RINGOUT_ROLLBACK_GATE_TESTING
  static RollbackHostEffectCoverage CompleteCoverageForTesting();
  explicit LiveRollbackOutputGate(RollbackHostEffectCoverage coverage);
#endif

  LiveRollbackOutputGate();
  ~LiveRollbackOutputGate() override;

  bool BeginHiddenReplay(u64 first_frame, u64 replay_through_frame) override;
  void EndHiddenReplay(bool corrected_frontier_is_publishable) override;

  bool HasCompleteCoverage() const;
  bool IsActive() const { return m_active; }
  bool IsFaulted() const { return m_faulted; }
  const RollbackHostEffectCoverage& GetCoverage() const { return m_coverage; }

private:
  explicit LiveRollbackOutputGate(RollbackHostEffectCoverage coverage, bool test_only);

  const RollbackHostEffectCoverage m_coverage;
  bool m_active = false;
  bool m_faulted = false;
};

}  // namespace NetPlay
