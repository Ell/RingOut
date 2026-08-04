// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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
}
