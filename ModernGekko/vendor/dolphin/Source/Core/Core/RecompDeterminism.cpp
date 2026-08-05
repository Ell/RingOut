// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/RecompDeterminism.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Hash.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"

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

  // Whole-RAM snapshot at one frame. A hash says two runs differ; only a dump
  // says where, and an address is what turns a divergence into a subsystem.
  std::string dump_path;
  u64 dump_frame = 0;

  // Write watch on one guest address, reported by the StaticRecomp core from
  // the module's write journal. Capped, because an address that turns out to be
  // written every frame would otherwise bury the run's output.
  u32 watch_address = 0;
  u32 watch_size = 4;
  u64 watch_max = 20;
  u64 watch_hits = 0;
  bool watch = false;

  // Per-frame poll of the watched word, alongside the journal. The journal only
  // sees stores made by recompiled guest code; DMA and anything the chassis
  // writes into guest RAM natively bypass it. Polling catches the change no
  // matter who made it, and pairing it with the store count for the same frame
  // is what distinguishes the two cases: a change with no store behind it did
  // not come from the guest CPU.
  u32 watch_last_value = 0;
  u64 watch_stores = 0;   // native stores seen since the previous frame
  u64 watch_changes = 0;  // changes reported, against the same cap

  // Scripted input: sorted by frame, each entry the pad state to hold from that
  // frame until the next one.
  struct PadFrame
  {
    u64 frame;
    u16 button;
    u8 stick_x, stick_y, substick_x, substick_y, trigger_l, trigger_r;
  };
  std::vector<PadFrame> input;
  std::size_t input_cursor = 0;
};

// Button names as they appear in a script, matched case-insensitively.
struct PadName
{
  const char* name;
  u16 bit;
};
constexpr PadName kPadNames[] = {
    {"A", PAD_BUTTON_A},         {"B", PAD_BUTTON_B},
    {"X", PAD_BUTTON_X},         {"Y", PAD_BUTTON_Y},
    {"Z", PAD_TRIGGER_Z},        {"START", PAD_BUTTON_START},
    {"L", PAD_TRIGGER_L},        {"R", PAD_TRIGGER_R},
    {"UP", PAD_BUTTON_UP},       {"DOWN", PAD_BUTTON_DOWN},
    {"LEFT", PAD_BUTTON_LEFT},   {"RIGHT", PAD_BUTTON_RIGHT},
};

// One line: "<frame> <buttons|-> [stick_x stick_y [substick_x substick_y [l r]]]".
// Buttons are comma-separated, '-' meaning none. Blank lines and '#' comments
// are skipped. Sticks default to centred (0x80) and triggers to 0.
bool ParseInputLine(const std::string& line, Config::PadFrame* out)
{
  std::string text = line.substr(0, line.find('#'));
  std::istringstream stream(text);
  std::string frame_token, buttons;
  if (!(stream >> frame_token >> buttons))
    return false;

  *out = Config::PadFrame{};
  out->frame = std::strtoull(frame_token.c_str(), nullptr, 10);
  out->stick_x = out->stick_y = out->substick_x = out->substick_y = 0x80;

  if (buttons != "-")
  {
    std::string name;
    std::istringstream names(buttons);
    while (std::getline(names, name, ','))
    {
      for (char& c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      for (const PadName& known : kPadNames)
      {
        if (name == known.name)
          out->button |= known.bit;
      }
    }
  }

  int value = 0;
  if (stream >> value) out->stick_x = static_cast<u8>(value);
  if (stream >> value) out->stick_y = static_cast<u8>(value);
  if (stream >> value) out->substick_x = static_cast<u8>(value);
  if (stream >> value) out->substick_y = static_cast<u8>(value);
  if (stream >> value) out->trigger_l = static_cast<u8>(value);
  if (stream >> value) out->trigger_r = static_cast<u8>(value);
  return true;
}

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

    if (const char* const dump = std::getenv("RINGOUT_DETERMINISM_DUMP"))
    {
      result.dump_path = dump;
      if (const char* const frame = std::getenv("RINGOUT_DETERMINISM_DUMP_FRAME"))
        result.dump_frame = std::strtoull(frame, nullptr, 10);
    }

    if (const char* const watch = std::getenv("RINGOUT_DETERMINISM_WATCH"))
    {
      // Base 0, so the address can be written the way the dump reports it.
      result.watch_address = static_cast<u32>(std::strtoull(watch, nullptr, 0));
      result.watch = result.watch_address != 0;
      if (const char* const size = std::getenv("RINGOUT_DETERMINISM_WATCH_SIZE"))
        result.watch_size = static_cast<u32>(std::strtoul(size, nullptr, 0));
      if (result.watch_size == 0)
        result.watch_size = 4;
      if (const char* const max = std::getenv("RINGOUT_DETERMINISM_WATCH_MAX"))
        result.watch_max = std::strtoull(max, nullptr, 10);
    }

    if (const char* const input = std::getenv("RINGOUT_DETERMINISM_INPUT"))
    {
      std::ifstream file(input);
      if (!file)
      {
        std::fprintf(stderr, "[input] cannot open %s; running with no input\n", input);
      }
      else
      {
        std::string line;
        while (std::getline(file, line))
        {
          Config::PadFrame entry;
          if (ParseInputLine(line, &entry))
            result.input.push_back(entry);
        }
        std::stable_sort(result.input.begin(), result.input.end(),
                         [](const Config::PadFrame& a, const Config::PadFrame& b) {
                           return a.frame < b.frame;
                         });
        std::fprintf(stderr, "[input] %zu scripted pad state(s) from %s\n",
                     result.input.size(), input);
      }
    }

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

bool IsWatchArmed()
{
  return GetConfig().watch;
}

u32 WatchGuestAddress()
{
  return GetConfig().watch_address;
}

u32 WatchSize()
{
  return GetConfig().watch_size;
}

void ReportWatchWrite(u32 block_pc, u32 lr, u32 size, u32 old_value, u64 timebase)
{
  Config& config = GetConfig();
  // Counted before the cap, so the per-frame attribution stays honest after the
  // reports go quiet.
  ++config.watch_stores;
  if (config.watch_hits >= config.watch_max)
    return;
  ++config.watch_hits;

  // stderr, not the hash log: the log is two numbers a script compares between
  // runs, and interleaving prose into it would break that. The frame is what
  // ties this line back to the log and the dump.
  std::fprintf(stderr,
               "[watch] frame=%llu %u-byte store to 0x%08X from block 0x%08X (lr=0x%08X) "
               "was=0x%08X tb=%llu\n",
               static_cast<unsigned long long>(s_frame), size, config.watch_address, block_pc, lr,
               old_value, static_cast<unsigned long long>(timebase));
  std::fflush(stderr);
  if (config.watch_hits == config.watch_max)
    std::fprintf(stderr, "[watch] report cap (%llu) reached; going quiet.\n",
                 static_cast<unsigned long long>(config.watch_max));
}

void PollWatch(const char* site, u32 pc, u32 value)
{
  Config& config = GetConfig();
  if (!config.watch || value == config.watch_last_value)
    return;

  if (config.watch_changes < config.watch_max)
  {
    ++config.watch_changes;
    std::fprintf(stderr,
                 "[watch] frame=%llu 0x%08X changed 0x%08X -> 0x%08X seen at %s (pc=0x%08X, "
                 "%llu native store(s) this frame)\n",
                 static_cast<unsigned long long>(s_frame), config.watch_address,
                 config.watch_last_value, value, site, pc,
                 static_cast<unsigned long long>(config.watch_stores));
    std::fflush(stderr);
  }
  config.watch_last_value = value;
}

bool ScriptedPad(int device_number, ::GCPadStatus* status)
{
  Config& config = GetConfig();
  // Port 1 only: the harness drives one controller, and silently feeding the
  // same script to every port would fake a four-player game.
  if (!config.active || config.input.empty() || device_number != 0 || status == nullptr)
    return false;

  // The script is sorted and s_frame only advances, so this walks forward once
  // over the run rather than searching per poll -- SI polls the pad several
  // times a frame.
  while (config.input_cursor + 1 < config.input.size() &&
         config.input[config.input_cursor + 1].frame <= s_frame)
  {
    ++config.input_cursor;
  }

  const Config::PadFrame& entry = config.input[config.input_cursor];
  // Nothing scripted yet: hold neutral rather than falling through to the host
  // pad, so a stray keypress cannot perturb a measurement run.
  const bool pending = entry.frame > s_frame;

  *status = GCPadStatus{};
  status->isConnected = true;
  status->button = static_cast<u16>((pending ? 0 : entry.button) | PAD_USE_ORIGIN);
  status->stickX = pending ? GCPadStatus::MAIN_STICK_CENTER_X : entry.stick_x;
  status->stickY = pending ? GCPadStatus::MAIN_STICK_CENTER_Y : entry.stick_y;
  status->substickX = pending ? GCPadStatus::C_STICK_CENTER_X : entry.substick_x;
  status->substickY = pending ? GCPadStatus::C_STICK_CENTER_Y : entry.substick_y;
  status->triggerLeft = pending ? 0 : entry.trigger_l;
  status->triggerRight = pending ? 0 : entry.trigger_r;
  return true;
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

  // Backstop poll. The core polls far more finely; this catches a change made
  // by anything that runs outside the native run loop entirely.
  if (config.watch)
  {
    const u32 offset = config.watch_address - 0x80000000u;
    if (offset + 4 <= ram_size)
    {
      const u8* const p = ram + offset;
      PollWatch("frame", 0, (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]});
    }
    config.watch_stores = 0;
  }

  // Dumped after the hash of the same frame, so the log line and the snapshot
  // describe identical state. Raw main RAM with no header: the file offset is
  // the offset into RAM, which makes the guest address 0x80000000 + offset and
  // lets cmp do the comparison without a tool of our own.
  if (!config.dump_path.empty() && s_frame == config.dump_frame)
  {
    if (std::FILE* const dump = std::fopen(config.dump_path.c_str(), "wb"))
    {
      std::fwrite(ram, 1, ram_size, dump);
      std::fclose(dump);
    }
    config.dump_path.clear();
  }

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
