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

namespace ExpansionInterface
{
enum class EXIDeviceType : int;
}

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
  BufferedAudioReconciliation,
  ControllerRumble,
  Achievements,
  MovieRecording,
  PersistentStorage,
  ReplayDerivedNetPlayOutbound,
  GuestNetwork,
  CorrectedFrontierPublication,
  Count,
};

constexpr std::size_t ROLLBACK_HOST_EFFECT_COUNT =
    static_cast<std::size_t>(RollbackHostEffect::Count);
using RollbackHostEffectCoverage = std::array<bool, ROLLBACK_HOST_EFFECT_COUNT>;

std::string_view GetRollbackHostEffectName(RollbackHostEffect effect);
bool IsLiveRollbackProductionReady();

// Production rollback currently supports the RingOut GameCube runtime only.
// Keep the runtime preflight explicit so a newly supported platform or host
// peripheral cannot silently inherit a capability claim it has not earned.
struct LiveRollbackProductionSessionPolicy
{
  bool is_gamecube_title = false;
  bool save_data_writable = true;
  bool sd_writes_allowed = true;
  bool memory_card_slots_safe = false;
  bool serial_port_1_disabled = false;
  bool serial_port_2_disabled = false;
  bool gba_devices_disabled = false;
};

bool IsLiveRollbackProductionSessionPolicySafe(const LiveRollbackProductionSessionPolicy& policy);
bool IsLiveRollbackMemoryCardSlotSafe(ExpansionInterface::EXIDeviceType device);

// Global read-only query used at narrow host-output choke points. The flag is
// published only after the complete capability matrix has passed.
bool IsLiveRollbackHiddenReplayActive();

// Replay-derived protocol traffic (input and timebase) must not be emitted
// from historical execution. User/control traffic remains live so peers can
// stop or disconnect a faulted session.
bool IsLiveRollbackReplayDerivedOutboundAllowed();

// Monotonic rollback epoch consumed by the sound thread. It lets the mixer
// discard queued speculative audio even if a short replay begins and commits
// between two backend Mix calls.
u64 GetLiveRollbackAudioResetGeneration();

// Session-lifetime quarantine for effects which cannot be reconciled after a
// wrong speculative frame and are therefore disabled for the whole rollback
// session: frame/audio dumping, controller rumble, achievement evaluation,
// movie recording, and persistent writes.
bool IsLiveRollbackSessionQuarantineActive();

// Releases suppression which a failed/cancelled replay deliberately leaves
// armed. Call only from the emulation teardown path after GPU, audio, and
// hardware output producers have stopped.
void FinalizeLiveRollbackOutputSuppressionAfterCoreTeardown();

class LiveRollbackOutputGate final : public RollbackOutputGate
{
public:
  // Coverage implemented in the current RingOut runtime. Newly added effects
  // default to uncovered, so production activation remains fail closed until
  // every enumerated effect has concrete enforcement.
  static RollbackHostEffectCoverage AuditedProductionCoverage();
  static std::unique_ptr<LiveRollbackOutputGate>
  CreateProductionGate(const LiveRollbackProductionSessionPolicy& policy);

  // Explicit escape hatch for the isolated two-process proof harness. It is
  // available only when RINGOUT_ROLLBACK_TEST_ACK=HEADLESS_ISOLATED, never from
  // ordinary lobby settings. The harness accepts that speculative host effects
  // are disposable so it can exercise real restore/resimulation end to end.
  static std::unique_ptr<LiveRollbackOutputGate> CreateHeadlessIsolatedTestGate();

#ifdef RINGOUT_ROLLBACK_GATE_TESTING
  static RollbackHostEffectCoverage CompleteCoverageForTesting();
  explicit LiveRollbackOutputGate(RollbackHostEffectCoverage coverage);
  void SetCorrectedFrontierBarrierForTesting(bool (*begin_barrier)(bool*),
                                             void (*end_barrier)(bool))
  {
    m_begin_corrected_frontier_barrier = begin_barrier;
    m_end_corrected_frontier_barrier = end_barrier;
  }
#endif

  LiveRollbackOutputGate();
  ~LiveRollbackOutputGate() override;

  bool BeginSessionQuarantine();
  void EndSessionQuarantine();
  bool BeginHiddenReplay(u64 first_frame, u64 replay_through_frame) override;
  bool EndHiddenReplay(bool corrected_frontier_is_publishable) override;

  bool HasCompleteCoverage() const;
  bool IsSessionQuarantineActive() const { return m_session_quarantine_active; }
  bool IsActive() const { return m_active; }
  bool IsFaulted() const { return m_faulted; }
  const RollbackHostEffectCoverage& GetCoverage() const { return m_coverage; }

private:
  explicit LiveRollbackOutputGate(RollbackHostEffectCoverage coverage, bool test_only);

  const RollbackHostEffectCoverage m_coverage;
  bool (*m_begin_corrected_frontier_barrier)(bool*) = nullptr;
  void (*m_end_corrected_frontier_barrier)(bool) = nullptr;
  bool m_session_quarantine_active = false;
  bool m_active = false;
  bool m_faulted = false;
  bool m_suppression_latched_until_teardown = false;
};

}  // namespace NetPlay
