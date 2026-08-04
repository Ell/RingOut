// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

// Per-frame state hashing, used to answer whether the statically recompiled
// core is deterministic -- the question netplay depends on entirely. Two runs
// from the same start with the same inputs must produce identical logs; the
// first frame where they differ is the desync.
//
// Inert unless RINGOUT_DETERMINISM_LOG names a file, so it costs one predicted
// branch per frame in a normal run.
namespace RecompDeterminism
{
// True when RINGOUT_DETERMINISM_LOG is set. The runtime consults this to force
// the emulation into a shape where a per-frame hash means anything -- see
// dolphin_runtime.cpp, which turns off the CPU thread when it is on.
bool IsActive();

void OnFrame(Core::System& system);

// Write watch. A dump says which address diverged; it cannot say what put the
// value there, and an address without a writer is not yet a subsystem. Setting
// RINGOUT_DETERMINISM_WATCH to a guest address makes the StaticRecomp core name
// the recompiled block that stores to it, which the symbol map turns into a
// function.
//
// Requires RINGOUT_DETERMINISM_LOG, like the rest of the harness. Optional:
// RINGOUT_DETERMINISM_WATCH_SIZE (bytes, default 4) and
// RINGOUT_DETERMINISM_WATCH_MAX (reports before it goes quiet, default 20).
bool IsWatchArmed();
u32 WatchGuestAddress();
u32 WatchSize();

// Reported by the core from inside the module's write journal, so `old_value`
// is the pre-image: the store has not committed yet.
void ReportWatchWrite(u32 block_pc, u32 lr, u32 size, u32 old_value, u64 timebase);

// Reports that the watched word changed to `value`, seen from `site`. The
// journal covers guest stores; this covers everyone else -- DMA, and anything
// the chassis writes into guest RAM natively -- by noticing the change after
// the fact. `site` is where the poll ran, which is the actual finding: a change
// first seen after CoreTiming::Advance came from a scheduled hardware event,
// while one seen after a dispatch came from that block's MMIO side effects.
// Callers filter on their own cached copy first, so this is off the hot path.
void PollWatch(const char* site, u32 pc, u32 value);
}
