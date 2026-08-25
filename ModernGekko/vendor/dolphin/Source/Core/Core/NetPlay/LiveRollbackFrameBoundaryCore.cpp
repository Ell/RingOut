// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/LiveRollbackFrameBoundary.h"

#include "Core/Core.h"

namespace NetPlay
{
namespace
{
LiveRollbackFrameBoundary* s_installed_driver = nullptr;
}

bool InstallLiveRollbackFrameBoundary(LiveRollbackFrameBoundary* const driver,
                                      const u64 initial_frame)
{
  if (!Core::IsCPUThread() || !driver || s_installed_driver)
    return false;
  if (driver->Activate(initial_frame) != LiveRollbackFrameBoundary::ActivationStatus::Active)
    return false;
  s_installed_driver = driver;
  return true;
}

void UninstallLiveRollbackFrameBoundary(LiveRollbackFrameBoundary* const driver)
{
  if (!Core::IsCPUThread() || s_installed_driver != driver)
    return;
  s_installed_driver->Deactivate();
  s_installed_driver = nullptr;
}

LiveRollbackFrameBoundary::BoundaryStatus CompleteLiveRollbackFrameBoundary()
{
  if (!s_installed_driver)
    return LiveRollbackFrameBoundary::BoundaryStatus::Inactive;
  return s_installed_driver->CompleteCurrentFrame();
}

}  // namespace NetPlay
