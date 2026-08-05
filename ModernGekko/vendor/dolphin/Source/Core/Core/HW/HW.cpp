// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>

#include "Core/HW/HW.h"

#include "Common/ChunkFile.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/HSP/HSP.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/MemoryInterface.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WII_IPC.h"
#include "Core/IOS/IOS.h"
#include "Core/State.h"
#include "Core/System.h"

namespace HW
{
void Init(Core::System& system, const Sram* override_sram)
{
  system.GetCoreTiming().Init();
  system.GetSystemTimers().PreInit();

  State::Init(system);

  // Init the whole Hardware
  system.GetAudioInterface().Init();
  system.GetVideoInterface().Init();
  system.GetSerialInterface().Init();
  system.GetProcessorInterface().Init();
  system.GetExpansionInterface().Init(override_sram);  // Needs to be initialized before Memory
  system.GetHSP().Init();
  system.GetMemory().Init();  // Needs to be initialized before AddressSpace
  AddressSpace::Init();
  system.GetMemoryInterface().Init();
  system.GetDSP().Init(Config::Get(Config::MAIN_DSP_HLE));
  system.GetDVDInterface().Init();
  system.GetGPFifo().Init();
  system.GetCPU().Init(Config::Get(Config::MAIN_CPU_CORE));
  system.GetSystemTimers().Init();

  if (system.IsWii())
  {
    system.GetWiiIPC().Init();
    IOS::HLE::Init(system);  // Depends on Memory
  }

  system.GetMemory().InitMMIO(system);
}

void Shutdown(Core::System& system)
{
  // IOS should always be shut down regardless of IsWii because it can be running in GC mode (MIOS).
  IOS::HLE::Shutdown(system);  // Depends on Memory
  system.GetWiiIPC().Shutdown();

  system.GetSystemTimers().Shutdown();
  system.GetCPU().Shutdown();
  system.GetDVDInterface().Shutdown();
  system.GetDSP().Shutdown();
  system.GetMemoryInterface().Shutdown();
  AddressSpace::Shutdown();
  system.GetMemory().Shutdown();
  system.GetHSP().Shutdown();
  system.GetExpansionInterface().Shutdown();
  system.GetSerialInterface().Shutdown();
  system.GetAudioInterface().Shutdown();

  State::Shutdown();
  system.GetCoreTiming().Shutdown();
}

void DoState(Core::System& system, PointerWrap& p)
{
  // Per-subsystem sizes, same one-shot env gate as the top-level breakdown in
  // State.cpp (RINGOUT_STATE_BREAKDOWN=1). HW is ~80 MiB of a ~106 MiB state,
  // so this is where a rollback snapshot would have to be narrowed.
  static const bool s_breakdown = std::getenv("RINGOUT_STATE_BREAKDOWN") != nullptr;
  static bool s_breakdown_done = false;
  const bool report = s_breakdown && !s_breakdown_done;
  u8* section_start = p.GetCurrentPosition();
  const auto section = [&](const char* name) {
    if (!report)
      return;
    const u32 bytes = p.GetOffsetFromPreviousPosition(section_start);
    if (bytes >= 4096)
      std::fprintf(stderr, "[state]   HW/%-14s %11u bytes  %7.2f MiB\n", name, bytes,
                   double(bytes) / (1024.0 * 1024.0));
    section_start = p.GetCurrentPosition();
  };

  system.GetMemory().DoState(p);
  p.DoMarker("Memory");
  section("Memory");
  system.GetMemoryInterface().DoState(p);
  p.DoMarker("MemoryInterface");
  section("MemoryInterface");
  system.GetVideoInterface().DoState(p);
  p.DoMarker("VideoInterface");
  section("VideoInterface");
  system.GetSerialInterface().DoState(p);
  p.DoMarker("SerialInterface");
  section("SerialInterface");
  system.GetProcessorInterface().DoState(p);
  p.DoMarker("ProcessorInterface");
  section("ProcessorInterface");
  system.GetDSP().DoState(p);
  p.DoMarker("DSP");
  section("DSP");
  system.GetDVDInterface().DoState(p);
  p.DoMarker("DVDInterface");
  section("DVDInterface");
  system.GetGPFifo().DoState(p);
  p.DoMarker("GPFifo");
  section("GPFifo");
  system.GetExpansionInterface().DoState(p);
  p.DoMarker("ExpansionInterface");
  section("ExpansionInterface");
  system.GetAudioInterface().DoState(p);
  p.DoMarker("AudioInterface");
  section("AudioInterface");
  system.GetHSP().DoState(p);
  p.DoMarker("HSP");
  section("HSP");

  if (report)
    s_breakdown_done = true;

  if (system.IsWii())
  {
    system.GetWiiIPC().DoState(p);
    p.DoMarker("IOS");
    system.GetIOS()->DoState(p);
    p.DoMarker("IOS::HLE");
  }

  p.DoMarker("WIIHW");
}
}  // namespace HW
