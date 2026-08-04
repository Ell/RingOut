// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/RecompDeterminism.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "Common/CommonTypes.h"
#include "Common/Hash.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace RecompDeterminism
{
namespace
{
// The OS globals block at the bottom of main memory: boot info, console type,
// and the RTC / time base the IPL seeds at boot. Everything above it is the
// game's own state.
constexpr u32 kOSGlobalsSize = 0x3100;

struct Config
{
  std::FILE* out = nullptr;
  u64 frames = 0;  // 0 = run until the game is stopped by hand
  bool active = false;
};

// Configured from the environment rather than the command line on purpose:
// this is a diagnostic for a question about the core, not a feature of the
// game, and it stays out of the runtime's option surface entirely.
Config& GetConfig()
{
  static Config config = [] {
    Config result;
    const char* const path = std::getenv("RINGOUT_DETERMINISM_LOG");
    if (path == nullptr || *path == '\0')
      return result;

    result.out = std::fopen(path, "w");
    if (result.out == nullptr)
      return result;

    if (const char* const frames = std::getenv("RINGOUT_DETERMINISM_FRAMES"))
      result.frames = std::strtoull(frames, nullptr, 10);

    result.active = true;
    return result;
  }();
  return config;
}

u64 s_frame = 0;
}  // namespace

bool IsActive()
{
  return GetConfig().active;
}

void OnFrame(Core::System& system)
{
  Config& config = GetConfig();
  if (!config.active)
    return;

  auto& memory = system.GetMemory();
  const u8* const ram = memory.GetRAM();
  if (ram == nullptr)
    return;

  // Hashed in two pieces, because "the RAM differs" is not a usable answer.
  //
  // The bottom of memory is the OS globals block: the boot info, the console
  // type and, critically, the real-time clock and time base the IPL seeds at
  // boot. Two runs started seconds apart differ there by construction, and
  // that is not a core defect -- it is why netplay pins the RTC across peers
  // rather than hoping. Splitting it out separates that expected difference
  // from a genuine one in the heap, where the game's actual state lives.
  const u32 ram_size = memory.GetRamSizeReal();
  const u32 low_size = std::min<u32>(kOSGlobalsSize, ram_size);
  const u32 low_hash = Common::ComputeCRC32(ram, low_size);
  const u32 rest_hash = Common::ComputeCRC32(ram + low_size, ram_size - low_size);
  u32 l1_hash = 0;
  if (const u8* const l1 = memory.GetL1Cache())
    l1_hash = Common::ComputeCRC32(l1, memory.GetL1CacheSize());

  std::fprintf(config.out, "%llu %08x %08x %08x\n", static_cast<unsigned long long>(s_frame),
               low_hash, rest_hash, l1_hash);
  // Flushed every frame because the interesting run is the one that diverges
  // and then crashes; a buffered tail would lose exactly the frames that matter.
  std::fflush(config.out);

  ++s_frame;
  if (config.frames != 0 && s_frame >= config.frames)
  {
    std::fclose(config.out);
    config.out = nullptr;
    config.active = false;
    // Stopping here would tear the core down from inside its own frame update,
    // so it goes through the host job queue instead.
    Core::QueueHostJob([](Core::System& stopping) { Core::Stop(stopping); });
  }
}
}  // namespace RecompDeterminism
