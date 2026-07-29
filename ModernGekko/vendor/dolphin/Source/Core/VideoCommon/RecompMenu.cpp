// Copyright 2026 ModernGekko Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/RecompMenu.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <imgui.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/Cheats/ActionReplay.h"
#include "Core/Cheats/GeckoCode.h"
#include "Core/Cheats/GeckoCodeConfig.h"
#include "Core/Cheats/PatchEngine.h"
#include "Core/Config/CheatSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/FreeLookSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/FreeLookManager.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

namespace RecompMenu
{
namespace
{
enum class Item
{
  // Video
  Widescreen,
  InternalRes,
  AspectRatio,
  VSync,
  AntiAliasing,
  Anisotropy,
  TextureFiltering,
  TexturePacks,
  PrefetchTextures,
  ShowFPS,
  FreeCamera,
  TrainingHud,
  LensFlares,
  Filter,
  Fullscreen,
  // Audio
  Volume,
  Muted,
  AudioLatency,
  FillGaps,
  // System
  Speed,
  StateSlot,
  SaveState,
  LoadState,
  AutoResume,
  Reset,
  Quit,
  // Shared
  Apply,
};

// Declaration order IS the on-screen tab order (Draw walks 0..kTabCount and
// Left/Right cycles modulo it), so System leads.
enum class Tab
{
  System,
  Video,
  Audio,
  Controls,
  Cheats,
  Count,
};

constexpr int kTabCount = static_cast<int>(Tab::Count);

const char* TabName(Tab tab)
{
  switch (tab)
  {
  case Tab::Video:
    return "VIDEO";
  case Tab::Audio:
    return "AUDIO";
  case Tab::System:
    return "SYSTEM";
  case Tab::Controls:
    return "CONTROLS";
  case Tab::Cheats:
    return "CHEATS";
  default:
    return "";
  }
}

// Controls and Cheats build their rows dynamically, so they have no static list.
const std::vector<Item>& TabItems(Tab tab)
{
  static const std::vector<Item> video = {
      Item::Widescreen,   Item::InternalRes, Item::AspectRatio,      Item::VSync,
      Item::AntiAliasing, Item::Anisotropy,  Item::TextureFiltering, Item::TexturePacks,
      Item::PrefetchTextures, Item::ShowFPS, Item::FreeCamera,       Item::TrainingHud,
      Item::LensFlares,       Item::Filter,      Item::Fullscreen,       Item::Apply};
  static const std::vector<Item> audio = {Item::Volume, Item::Muted, Item::AudioLatency,
                                          Item::FillGaps, Item::Apply};
  static const std::vector<Item> system = {Item::Speed,      Item::StateSlot, Item::SaveState,
                                           Item::LoadState,  Item::AutoResume,
                                           Item::Apply,      Item::Reset,     Item::Quit};
  static const std::vector<Item> none = {};

  switch (tab)
  {
  case Tab::Video:
    return video;
  case Tab::Audio:
    return audio;
  case Tab::System:
    return system;
  default:
    return none;
  }
}

// One cheat code flattened for the list: either an Action Replay or a Gecko
// entry, identified by index into the matching vector.
struct CheatRow
{
  std::string label;
  bool gecko = false;
  size_t index = 0;
};

// One remappable input on the emulated GC pad, flattened out of the
// group/control tree so the page is a simple list.
struct ControlRow
{
  std::string label;
  ControllerEmu::Control* control = nullptr;
};

struct State
{
  std::mutex mutex;
  bool open = false;
  int selected = 0;
  int state_slot = 1;
  std::function<void()> fullscreen_callback;
  std::function<void()> quit_callback;

  // Row 0 is always the tab selector; rows 1.. are that tab's entries.
  Tab tab = Tab::System;
  std::vector<ControlRow> control_rows;

  // Input detection is asynchronous: Start() then Update() until IsComplete(),
  // driven from PumpFrame so the overlay keeps redrawing while we wait.
  std::unique_ptr<ciface::Core::InputDetector> detector;
  ControllerEmu::Control* detecting_control = nullptr;

  std::vector<ActionReplay::ARCode> ar_codes;
  std::vector<Gecko::GeckoCode> gecko_codes;
  std::vector<CheatRow> cheat_rows;
};

State s_state;

// Entries below the tab selector for the active tab.
int RowCount(const State& state)
{
  switch (state.tab)
  {
  case Tab::Controls:
    return static_cast<int>(state.control_rows.size());
  case Tab::Cheats:
    return 1 + static_cast<int>(state.cheat_rows.size());  // master switch + codes
  default:
    return static_cast<int>(TabItems(state.tab).size());
  }
}

// Pausing must NOT happen on the host thread. CPUManager::SetStepping(true)
// blocks until the CPU thread acknowledges, but the host thread is also the
// Wayland/X11 event loop: CPU thread waits on the video thread, the video
// thread waits in Present for a swapchain image, and that needs compositor
// events which only the host thread can dispatch. Blocking it closes the loop
// and the window wedges -- which is why mashing Escape triggered it.
//
// So state changes are handed to a dedicated worker. It blocks instead, the
// host thread keeps pumping events, and the deadlock cannot form. Rapid toggles
// are latest-wins, which is exactly right for pause/resume.
std::mutex s_core_state_mutex;
std::condition_variable s_core_state_cv;
bool s_core_state_want_paused = false;
bool s_core_state_pending = false;
bool s_core_state_worker_started = false;

// Settings staged while the menu is open. Config::SetBase still writes straight
// away (so a row shows its new value immediately), but the callbacks that make a
// change actually TAKE EFFECT -- VideoConfig::Refresh and friends -- are held
// back by this guard. Applying a backend reconfigure while the core is paused is
// what used to wedge the video thread inside VKTexture::TransitionToLayout, so
// the guard is released by the worker only AFTER the resume has gone through:
// unpause first, then apply. Guarded by s_core_state_mutex.
// unique_ptr, not optional: ConfigChangeCallbackGuard is deliberately neither
// copyable nor movable, so it can only be handed around behind a pointer.
std::unique_ptr<Config::ConfigChangeCallbackGuard> s_settings_guard;

void CoreStateWorker()
{
  for (;;)
  {
    bool want_paused;
    {
      std::unique_lock<std::mutex> lock(s_core_state_mutex);
      s_core_state_cv.wait(lock, [] { return s_core_state_pending; });
      want_paused = s_core_state_want_paused;
      s_core_state_pending = false;
    }

    auto& system = Core::System::GetInstance();
    if (Core::IsRunning(system))
      Core::SetState(system, want_paused ? Core::State::Paused : Core::State::Running);

    if (!want_paused)
    {
      // Released here, off the host thread and after the core is running again.
      // Destroying the guard is what dispatches the staged config callbacks.
      // (Unconditional, so a resume requested while the core is not running
      // still flushes rather than stranding the settings forever.)
      std::unique_ptr<Config::ConfigChangeCallbackGuard> release;
      {
        std::lock_guard<std::mutex> lock(s_core_state_mutex);
        release = std::move(s_settings_guard);
      }
      release.reset();   // fires the staged callbacks, outside the lock
    }
  }
}

void RequestCoreState(bool paused)
{
  std::lock_guard<std::mutex> lock(s_core_state_mutex);
  if (!s_core_state_worker_started)
  {
    s_core_state_worker_started = true;
    std::thread(CoreStateWorker).detach();
  }
  s_core_state_want_paused = paused;
  s_core_state_pending = true;
  s_core_state_cv.notify_one();
}

bool IsSelectable(Item item)
{
  return true;
}

// Opt-in continue-where-you-left-off: the Quit row saves a state to a fixed
// file and the next boot loads it once the core is running.
const Config::Info<bool> RECOMP_AUTO_RESUME{{Config::System::Main, "RecompMenu", "AutoResume"},
                                            false};

std::string AutoResumePath()
{
  return File::GetUserPath(D_STATESAVES_IDX) + "autoresume.sav";
}

// ---- Training HUD ---------------------------------------------------------
// Live match state read straight out of guest MEM1. Anchor addresses come from
// the decrypted retail AR codes ("P1 Unlimited Health" writes f32 240.0 to
// 0x8034EA1C; "Unlimited Time" writes u8 99 to 0x8038F6D7), so health is an
// f32 with 240.0 = full bar.
constexpr u32 kHudP1Health = 0x8034EA1C;
constexpr u32 kHudTimer = 0x8038F6D7;
constexpr float kHudFullHealth = 240.0f;

const Config::Info<bool> RECOMP_TRAINING_HUD{{Config::System::Main, "RecompMenu", "TrainingHud"},
                                             false};
// P2's health has no AR-code anchor. 0x8036FABC was proven by differential
// scan during a real two-sided match: of every f32 in MEM1 at 240.0 at round
// start, only it and P1's slot ever decreased, and each moved only when its
// own fighter was hit. (First guess 0x8034EA28, "the 240.0 next to P1", never
// tracked damage.) Config-overridable; 0 hides the P2 side entirely.
const Config::Info<u32> RECOMP_HUD_P2_ADDR{{Config::System::Main, "RecompMenu", "TrainingHudP2"},
                                           0x8036FABCu};

const u8* HudGuestRam(u32* out_size)
{
  auto& system = Core::System::GetInstance();
  if (!Core::IsRunning(system))
    return nullptr;
  auto& memory = system.GetMemory();
  *out_size = memory.GetRamSizeReal();
  return memory.GetRAM();
}

u32 HudReadU32(const u8* ram, u32 ram_size, u32 addr)
{
  const u32 off = addr & 0x01FFFFFFu;
  if (ram == nullptr || off + 4 > ram_size)
    return 0;
  return (u32(ram[off]) << 24) | (u32(ram[off + 1]) << 16) | (u32(ram[off + 2]) << 8) |
         u32(ram[off + 3]);
}

float HudReadF32(const u8* ram, u32 ram_size, u32 addr)
{
  return std::bit_cast<float>(HudReadU32(ram, ram_size, addr));
}

u8 HudReadU8(const u8* ram, u32 ram_size, u32 addr)
{
  const u32 off = addr & 0x01FFFFFFu;
  if (ram == nullptr || off >= ram_size)
    return 0;
  return ram[off];
}

// Per-player damage tracking. Video-thread-only state: Draw is the sole caller.
struct HudTrack
{
  float prev = -1.0f;
  float last_hit = 0.0f;
  float combo = 0.0f;
  std::chrono::steady_clock::time_point last_hit_at{};

  void Update(float health)
  {
    const auto now = std::chrono::steady_clock::now();
    if (prev >= 0.0f && health < prev)
    {
      const float dmg = prev - health;
      last_hit = dmg;
      // Hits close together read as one combo; a gap starts a new one.
      if (now - last_hit_at <= std::chrono::milliseconds(1200))
        combo += dmg;
      else
        combo = dmg;
      last_hit_at = now;
    }
    else if (prev >= 0.0f && health > prev + 1.0f)
    {
      // Round reset / heal: start over but keep the last numbers on screen.
      combo = 0.0f;
    }
    prev = health;
  }
};

void HudBar(const char* label, float health, bool right_to_left)
{
  const float frac = std::clamp(health / kHudFullHealth, 0.0f, 1.0f);
  char text[48];
  std::snprintf(text, sizeof(text), "%s  %.0f / %.0f", label, std::max(health, 0.0f),
                kHudFullHealth);
  // Health drains toward the middle like the game's own bars: P2's fill is
  // mirrored by right-aligning a spacer before the fill.
  ImVec4 color = frac > 0.5f ? ImVec4(0.20f, 0.75f, 0.25f, 1.0f) :
                 frac > 0.25f ? ImVec4(0.85f, 0.70f, 0.15f, 1.0f) :
                                ImVec4(0.85f, 0.20f, 0.15f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
  ImGui::ProgressBar(frac, ImVec2(230.0f, 16.0f), text);
  ImGui::PopStyleColor();
  (void)right_to_left;
}

void DrawTrainingHud()
{
  static const bool env_force = std::getenv("RECOMP_TRAINING_HUD") != nullptr;
  static const bool env_scan = std::getenv("RECOMP_HUD_SCAN") != nullptr;
  if (!env_force && !Config::Get(RECOMP_TRAINING_HUD))
    return;

  u32 ram_size = 0;
  const u8* ram = HudGuestRam(&ram_size);
  if (ram == nullptr)
    return;

  const float p1 = HudReadF32(ram, ram_size, kHudP1Health);
  const u32 p2_addr = Config::Get(RECOMP_HUD_P2_ADDR);
  const float p2 = p2_addr != 0 ? HudReadF32(ram, ram_size, p2_addr) : -1.0f;
  const u8 timer = HudReadU8(ram, ram_size, kHudTimer);

  if (env_scan)
  {
    // Differential health hunt (the first, exact-240-in-a-window version
    // fingered a slot that never moved — 0x8034EA28 turned out not to track
    // P2 damage). Seed on EVERY f32 in MEM1 holding exactly 240.0, then each
    // pass keep only candidates that stay in health range; the ones that also
    // DECREASE while both fighters take hits are the real health variables.
    struct Cand
    {
      u32 addr;
      float last;
      bool dropped;
    };
    static std::vector<Cand> s_cands;
    static bool s_seeded = false;
    static int s_frame = 0;
    if (++s_frame % 120 == 0)
    {
      if (!s_seeded)
      {
        // Wait for a round start: P1's known slot reads exactly 240.0 there,
        // so every other health variable is guaranteed to be at 240.0 too.
        if (p1 == kHudFullHealth)
        {
          for (u32 addr = 0x80003000u; addr < 0x80000000u + ram_size; addr += 4)
          {
            if (HudReadU32(ram, ram_size, addr) == 0x43700000u)
              s_cands.push_back({addr, kHudFullHealth, false});
          }
          s_seeded = true;
          std::fprintf(stderr, "[hud-scan] seeded %zu candidates at 240.0\n", s_cands.size());
        }
      }
      else
      {
        std::erase_if(s_cands, [&](Cand& c) {
          const float v = HudReadF32(ram, ram_size, c.addr);
          if (!(v >= 0.0f && v <= kHudFullHealth + 0.5f))
            return true;  // left health range: junk or reused memory
          if (v < c.last - 0.5f)
            c.dropped = true;
          c.last = v;
          return false;
        });
        int shown = 0;
        for (const Cand& c : s_cands)
        {
          if (c.dropped && shown++ < 16)
            std::fprintf(stderr, "[hud-scan] HEALTH-LIKE %08X now=%.1f\n", c.addr, c.last);
        }
        std::fprintf(stderr, "[hud-scan] pass: %zu candidates, p1=%.1f timer=%u\n", s_cands.size(),
                     p1, timer);
      }
    }
  }

  // Only meaningful inside a match; the health slot holds junk elsewhere.
  const bool plausible = p1 >= 0.0f && p1 <= kHudFullHealth + 1.0f;
  if (!plausible)
    return;

  static HudTrack s_track_p1, s_track_p2;
  s_track_p1.Update(p1);
  if (p2 >= 0.0f)
    s_track_p2.Update(p2);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 30.0f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.55f);
  ImGui::Begin("TrainingHud", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

  HudBar("P1", p1, false);
  ImGui::SameLine();
  ImGui::Text(" %02u ", timer);
  if (p2 >= 0.0f)
  {
    ImGui::SameLine();
    HudBar("P2", p2, true);
  }

  ImGui::Text("last %.1f  combo %.1f", s_track_p1.last_hit, s_track_p1.combo);
  if (p2 >= 0.0f)
  {
    ImGui::SameLine(0.0f, 120.0f);
    ImGui::Text("last %.1f  combo %.1f", s_track_p2.last_hit, s_track_p2.combo);
  }
  ImGui::End();
}

const char* ItemLabel(Item item)
{
  switch (item)
  {
  case Item::Widescreen:
    return "Widescreen (16:9)";
  case Item::InternalRes:
    return "Internal Resolution";
  case Item::AspectRatio:
    return "Aspect Ratio";
  case Item::LensFlares:
    return "Lens Flares";
  case Item::Filter:
    return "Filter";
  case Item::VSync:
    return "V-Sync";
  case Item::AntiAliasing:
    return "Anti-Aliasing";
  case Item::Anisotropy:
    return "Anisotropic Filter";
  case Item::TextureFiltering:
    return "Texture Filtering";
  case Item::TexturePacks:
    return "Texture Packs";
  case Item::PrefetchTextures:
    return "Prefetch Textures";
  case Item::ShowFPS:
    return "Show FPS";
  case Item::FreeCamera:
    return "Free Camera";
  case Item::TrainingHud:
    return "Training HUD";
  case Item::Fullscreen:
    return "Fullscreen";
  case Item::Volume:
    return "Volume";
  case Item::Muted:
    return "Mute";
  case Item::AudioLatency:
    return "Audio Latency";
  case Item::FillGaps:
    return "Fill Audio Gaps";
  case Item::Speed:
    return "Emulation Speed";
  case Item::StateSlot:
    return "State Slot";
  case Item::SaveState:
    return "Save State";
  case Item::LoadState:
    return "Load State";
  case Item::AutoResume:
    return "Auto-Resume";
  case Item::Reset:
    return "Reset Game";
  case Item::Quit:
    return "Quit Game";
  case Item::Apply:
    return "Apply / Save Settings";
  default:
    return "";
  }
}

// Post-processing filters offered by the Filter row. Dolphin ships 48 shaders,
// which is far too many to page through one key press at a time, so this is a
// curated set: the two written for this project (Dolphin has no scanline or CRT
// filter of its own) plus the generally useful ones already present.
//
// The empty string is Dolphin's own representation of "no shader", not a
// placeholder of ours.
struct FilterEntry
{
  const char* label;
  const char* shader;
};

constexpr std::array<FilterEntry, 8> kFilters = {{
    {"Off", ""},
    {"Scanlines", "scanlines"},
    {"CRT", "crt"},
    {"FXAA", "FXAA"},
    {"Grayscale", "grayscale"},
    {"Sepia", "sepia"},
    {"Posterize", "posterize"},
    {"Invert", "invert"},
}};

std::string ItemValue(Item item, int state_slot)
{
  switch (item)
  {
  case Item::Widescreen:
    return Config::Get(Config::GFX_WIDESCREEN_HACK) ? "ON" : "OFF";
  case Item::InternalRes:
    return std::to_string(Config::Get(Config::GFX_EFB_SCALE)) + "x";
  case Item::AspectRatio:
    switch (Config::Get(Config::GFX_ASPECT_RATIO))
    {
    case AspectMode::ForceWide:
      return "16:9";
    case AspectMode::ForceStandard:
      return "4:3";
    case AspectMode::Stretch:
      return "Stretch";
    default:
      return "Auto";
    }
  case Item::LensFlares:
    // Dolphin issue 10475: the sun's occlusion test reads back an EFB copy, so
    // "Store EFB Copies to Texture Only" (GFX_HACK_SKIP_EFB_COPY_TO_RAM) hides
    // lens flares. Flares ON therefore means the hack is OFF, which costs some
    // performance -- hence the toggle.
    return Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM) ? "OFF" : "ON";
  case Item::VSync:
    return Config::Get(Config::GFX_VSYNC) ? "ON" : "OFF";
  case Item::AntiAliasing:
  {
    const u32 samples = Config::Get(Config::GFX_MSAA);
    if (samples <= 1)
      return "None";
    return std::to_string(samples) + "x " + (Config::Get(Config::GFX_SSAA) ? "SSAA" : "MSAA");
  }
  case Item::Anisotropy:
    switch (Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY))
    {
    case AnisotropicFilteringMode::Force1x:
      return "1x";
    case AnisotropicFilteringMode::Force2x:
      return "2x";
    case AnisotropicFilteringMode::Force4x:
      return "4x";
    case AnisotropicFilteringMode::Force8x:
      return "8x";
    case AnisotropicFilteringMode::Force16x:
      return "16x";
    default:
      return "Default";
    }
  case Item::TextureFiltering:
    switch (Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING))
    {
    case TextureFilteringMode::Nearest:
      return "Nearest";
    case TextureFilteringMode::Linear:
      return "Linear";
    default:
      return "Default";
    }
  case Item::Filter:
  {
    const std::string current = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
    for (const FilterEntry& filter : kFilters)
      if (current == filter.shader)
        return filter.label;
    // Something set it to a shader outside the curated list -- show its name
    // rather than lying about it being off.
    return current.empty() ? "Off" : current;
  }
  case Item::TexturePacks:
    return Config::Get(Config::GFX_HIRES_TEXTURES) ? "ON" : "OFF";
  case Item::PrefetchTextures:
    return Config::Get(Config::GFX_CACHE_HIRES_TEXTURES) ? "ON" : "OFF";
  case Item::ShowFPS:
    return Config::Get(Config::GFX_SHOW_FPS) ? "ON" : "OFF";
  case Item::FreeCamera:
    return Config::Get(Config::FREE_LOOK_ENABLED) ? "ON" : "OFF";
  case Item::TrainingHud:
    return Config::Get(RECOMP_TRAINING_HUD) ? "ON" : "OFF";
  case Item::Volume:
    return std::to_string(Config::Get(Config::MAIN_AUDIO_VOLUME));
  case Item::Muted:
    return Config::Get(Config::MAIN_AUDIO_MUTED) ? "ON" : "OFF";
  case Item::AudioLatency:
    return std::to_string(Config::Get(Config::MAIN_AUDIO_LATENCY)) + " ms";
  case Item::FillGaps:
    return Config::Get(Config::MAIN_AUDIO_FILL_GAPS) ? "ON" : "OFF";
  case Item::Speed:
  {
    const float speed = Config::Get(Config::MAIN_EMULATION_SPEED);
    if (speed <= 0.0f)
      return "Unlimited";
    return std::to_string(static_cast<int>(speed * 100.0f + 0.5f)) + "%";
  }
  case Item::StateSlot:
    return std::to_string(state_slot);
  case Item::AutoResume:
    return Config::Get(RECOMP_AUTO_RESUME) ? "ON" : "OFF";
  default:
    return "";
  }
}

ControllerEmu::EmulatedController* GetPad()
{
  InputConfig* const config = Pad::GetConfig();
  if (config == nullptr || config->GetControllerCount() == 0)
    return nullptr;
  return config->GetController(0);
}

// Flattens the pad's group/control tree into the page's row list. Only inputs
// are remappable; outputs (rumble) are skipped.
// Touches InputConfig, so likewise runs with the menu mutex released.
void BuildControlRowsData(std::vector<ControlRow>* rows)
{
  rows->clear();
  auto* const pad = GetPad();
  if (pad == nullptr)
    return;

  for (auto& group : pad->groups)
  {
    for (auto& control : group->controls)
    {
      if (control->control_ref == nullptr || !control->control_ref->IsInput())
        continue;
      rows->push_back({group->ui_name + ": " + control->ui_name, control.get()});
    }
  }
}

void StartDetection(State& state, ControllerEmu::Control* control)
{
  auto* const pad = GetPad();
  if (pad == nullptr || control == nullptr)
    return;

  const std::vector<std::string> devices = {pad->GetDefaultDevice().ToString()};
  state.detector = std::make_unique<ciface::Core::InputDetector>();
  state.detector->Start(g_controller_interface, devices);
  state.detecting_control = control;
}

// Reads the game's Action Replay + Gecko codes from its ini (built-in defaults
// plus the user's GameSettings/<id>.ini) into the page's list. Codes are stored
// with their saved enabled state; a game with no codes yields an empty list.
// Does game-ini file I/O, so it must run with the menu mutex RELEASED -- the
// video thread blocks on that mutex inside Draw(). Results are assigned to the
// State afterwards under a short lock.
void LoadCheatCodesData(std::vector<ActionReplay::ARCode>* ar_codes,
                        std::vector<Gecko::GeckoCode>* gecko_codes,
                        std::vector<CheatRow>* cheat_rows)
{
  const SConfig& sconfig = SConfig::GetInstance();
  const Common::IniFile global_ini = sconfig.LoadDefaultGameIni();
  const Common::IniFile local_ini = sconfig.LoadLocalGameIni();

  *ar_codes = ActionReplay::LoadCodes(global_ini, local_ini);
  *gecko_codes = Gecko::LoadCodes(global_ini, local_ini);

  // RE aid: the shipped codes are Codejunkies-encrypted in the ini, and the
  // decrypted address/value pairs are what point at live game state (health,
  // timer, ...) for things like the training HUD.
  if (std::getenv("RECOMP_DUMP_ARCODES"))
  {
    for (const auto& code : *ar_codes)
    {
      std::fprintf(stderr, "[arcode] %s\n", code.name.c_str());
      for (const auto& op : code.ops)
        std::fprintf(stderr, "[arcode]   %08X %08X\n", op.cmd_addr, op.value);
    }
  }

  cheat_rows->clear();
  for (size_t i = 0; i < ar_codes->size(); ++i)
    cheat_rows->push_back({"[AR] " + (*ar_codes)[i].name, false, i});
  for (size_t i = 0; i < gecko_codes->size(); ++i)
    cheat_rows->push_back({"[Gecko] " + (*gecko_codes)[i].name, true, i});
}

// Pushes the current enabled flags to the live cheat engine. RunAllActive fires
// each frame from a VI-timed CoreTiming event, so this takes effect immediately
// without a reboot. Turning any code on also flips the master cheats switch so
// the state is coherent.
// Takes copies rather than the live State so it can run with the menu mutex
// released; the caller flushes Config afterwards.
void ApplyCheatCodes(const std::vector<ActionReplay::ARCode>& ar_codes,
                     const std::vector<Gecko::GeckoCode>& gecko_codes)
{
  const SConfig& sconfig = SConfig::GetInstance();
  const std::string game_id = sconfig.GetGameID();
  const u16 revision = sconfig.GetRevision();

  ActionReplay::ApplyCodes(ar_codes, game_id, revision);
  Gecko::SetActiveCodes(gecko_codes, game_id, revision);
}

// Widescreen needs the projection widened, not the 4:3 image stretched, so the
// hack and the aspect mode are always flipped together (same pairing as Alt+W).
void SetWidescreen(bool enable)
{
  Config::SetBase(Config::GFX_WIDESCREEN_HACK, enable);
  Config::SetBase(Config::GFX_ASPECT_RATIO, enable ? AspectMode::ForceWide : AspectMode::Auto);
}

// Returns true if a Config value changed and needs flushing. The flush itself
// (Config::Save) must happen with the menu mutex released -- see OnKey.
bool AdjustItem(Item item, int direction, State& state)
{
  switch (item)
  {
  case Item::Widescreen:
    SetWidescreen(!Config::Get(Config::GFX_WIDESCREEN_HACK));
    break;
  case Item::InternalRes:
  {
    const int scale = std::clamp(Config::Get(Config::GFX_EFB_SCALE) + direction, 1, 8);
    Config::SetBase(Config::GFX_EFB_SCALE, scale);
    break;
  }
  case Item::AspectRatio:
  {
    static constexpr std::array<AspectMode, 4> kModes = {
        AspectMode::Auto, AspectMode::ForceWide, AspectMode::ForceStandard, AspectMode::Stretch};
    const AspectMode current = Config::Get(Config::GFX_ASPECT_RATIO);
    int index = 0;
    for (size_t i = 0; i < kModes.size(); ++i)
      if (kModes[i] == current)
        index = static_cast<int>(i);
    index = std::clamp(index + direction, 0, static_cast<int>(kModes.size()) - 1);
    Config::SetBase(Config::GFX_ASPECT_RATIO, kModes[index]);
    break;
  }
  case Item::LensFlares:
    Config::SetBase(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM,
                    !Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM));
    break;
  case Item::VSync:
    Config::SetBase(Config::GFX_VSYNC, !Config::Get(Config::GFX_VSYNC));
    break;
  case Item::AntiAliasing:
  {
    // Sample counts the backends reliably support; SSAA is left off.
    static constexpr std::array<u32, 4> kSamples = {1, 2, 4, 8};
    const u32 current = Config::Get(Config::GFX_MSAA);
    int index = 0;
    for (size_t i = 0; i < kSamples.size(); ++i)
      if (kSamples[i] == current)
        index = static_cast<int>(i);
    index = std::clamp(index + direction, 0, static_cast<int>(kSamples.size()) - 1);
    Config::SetBase(Config::GFX_MSAA, kSamples[index]);
    Config::SetBase(Config::GFX_SSAA, false);
    break;
  }
  case Item::Anisotropy:
  {
    static constexpr std::array<AnisotropicFilteringMode, 6> kModes = {
        AnisotropicFilteringMode::Default, AnisotropicFilteringMode::Force1x,
        AnisotropicFilteringMode::Force2x, AnisotropicFilteringMode::Force4x,
        AnisotropicFilteringMode::Force8x, AnisotropicFilteringMode::Force16x};
    const AnisotropicFilteringMode current = Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY);
    int index = 0;
    for (size_t i = 0; i < kModes.size(); ++i)
      if (kModes[i] == current)
        index = static_cast<int>(i);
    index = std::clamp(index + direction, 0, static_cast<int>(kModes.size()) - 1);
    Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY, kModes[index]);
    break;
  }
  case Item::TextureFiltering:
  {
    static constexpr std::array<TextureFilteringMode, 3> kModes = {
        TextureFilteringMode::Default, TextureFilteringMode::Nearest, TextureFilteringMode::Linear};
    const TextureFilteringMode current = Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING);
    int index = 0;
    for (size_t i = 0; i < kModes.size(); ++i)
      if (kModes[i] == current)
        index = static_cast<int>(i);
    index = std::clamp(index + direction, 0, static_cast<int>(kModes.size()) - 1);
    Config::SetBase(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING, kModes[index]);
    break;
  }
  case Item::Filter:
  {
    const std::string current = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
    int index = 0;
    for (size_t i = 0; i < kFilters.size(); ++i)
      if (current == kFilters[i].shader)
        index = static_cast<int>(i);
    index = std::clamp(index + direction, 0, static_cast<int>(kFilters.size()) - 1);
    Config::SetBase(Config::GFX_ENHANCE_POST_SHADER, std::string(kFilters[index].shader));
    break;
  }
  case Item::TexturePacks:
    // Dolphin-format packs, loaded from <userdir>/Load/Textures/GRSEAF/.
    Config::SetBase(Config::GFX_HIRES_TEXTURES, !Config::Get(Config::GFX_HIRES_TEXTURES));
    break;
  case Item::PrefetchTextures:
    // Loads the whole pack into RAM up front: smoother, but a big pack costs a
    // long load and a lot of memory.
    Config::SetBase(Config::GFX_CACHE_HIRES_TEXTURES,
                    !Config::Get(Config::GFX_CACHE_HIRES_TEXTURES));
    break;
  case Item::ShowFPS:
    Config::SetBase(Config::GFX_SHOW_FPS, !Config::Get(Config::GFX_SHOW_FPS));
    break;
  case Item::FreeCamera:
    Config::SetBase(Config::FREE_LOOK_ENABLED, !Config::Get(Config::FREE_LOOK_ENABLED));
    break;
  case Item::TrainingHud:
    Config::SetBase(RECOMP_TRAINING_HUD, !Config::Get(RECOMP_TRAINING_HUD));
    break;
  case Item::Muted:
    Config::SetBase(Config::MAIN_AUDIO_MUTED, !Config::Get(Config::MAIN_AUDIO_MUTED));
    break;
  case Item::AudioLatency:
    Config::SetBase(Config::MAIN_AUDIO_LATENCY,
                    std::clamp(Config::Get(Config::MAIN_AUDIO_LATENCY) + direction * 5, 0, 200));
    break;
  case Item::FillGaps:
    Config::SetBase(Config::MAIN_AUDIO_FILL_GAPS, !Config::Get(Config::MAIN_AUDIO_FILL_GAPS));
    break;
  case Item::Volume:
  {
    const int volume = std::clamp(Config::Get(Config::MAIN_AUDIO_VOLUME) + direction * 5, 0, 100);
    Config::SetBase(Config::MAIN_AUDIO_VOLUME, volume);
    break;
  }
  case Item::Speed:
  {
    // 0 (unlimited) sits above 200% so the wrap order reads naturally.
    static constexpr std::array<float, 8> kSpeeds = {0.25f, 0.5f,  0.75f, 1.0f,
                                                     1.25f, 1.5f,  2.0f,  0.0f};
    const float current = Config::Get(Config::MAIN_EMULATION_SPEED);
    int index = 3;
    for (size_t i = 0; i < kSpeeds.size(); ++i)
    {
      if (kSpeeds[i] == current)
      {
        index = static_cast<int>(i);
        break;
      }
    }
    index = std::clamp(index + direction, 0, static_cast<int>(kSpeeds.size()) - 1);
    Config::SetBase(Config::MAIN_EMULATION_SPEED, kSpeeds[index]);
    break;
  }
  case Item::AutoResume:
    // On quit, save a state; on next launch, load it. Save/load themselves
    // happen outside the mutex (Quit action / ScheduleAutoResumeLoad).
    Config::SetBase(RECOMP_AUTO_RESUME, !Config::Get(RECOMP_AUTO_RESUME));
    break;
  case Item::StateSlot:
    state.state_slot = std::clamp(state.state_slot + direction, 1, 8);
    return false;  // Not a Config setting; nothing to persist.
  default:
    return false;
  }

  // Writes went to the base layer above; the caller flushes them once it has
  // dropped the mutex.
  return true;
}

// Activating some rows means calling into Core, which must never happen while
// the menu mutex is held (Draw takes the same lock from the present path) and,
// for save states, must not happen while emulation is paused: State::Save goes
// through Core::RunOnCPUThread, whose PauseAndLock/RestoreStateAndUnlock pair
// restores whatever run state it found. Called with the core already paused it
// sees was_running == false, leaves the CPU paused, and the queued job never
// runs -- which wedges the caller. So Activate only *decides*; OnKey performs.
enum class Action
{
  None,
  Fullscreen,
  Quit,
  Reset,
  SaveState,
  LoadState,
};

Action DecideAction(Item item, State& state, bool* needs_config_save)
{
  switch (item)
  {
  case Item::Fullscreen:
    return Action::Fullscreen;
  case Item::SaveState:
    return Action::SaveState;
  case Item::LoadState:
    return Action::LoadState;
  case Item::Reset:
    return Action::Reset;
  case Item::Quit:
    return Action::Quit;
  case Item::Apply:
    // Settings already take effect as they are changed; this flushes them to
    // disk and gives an explicit confirm step.
    *needs_config_save = true;
    return Action::None;
  default:
    // Enter deliberately does NOT cycle values -- Left/Right do that. Enter used
    // to double as "next value", which made it far too easy to change a setting
    // while trying to confirm one.
    return Action::None;
  }
}
}  // namespace

bool IsOpen()
{
  std::lock_guard<std::mutex> guard(s_state.mutex);
  return s_state.open;
}

void Toggle()
{
  bool open_now;
  {
    std::lock_guard<std::mutex> guard(s_state.mutex);
    s_state.open = !s_state.open;
    open_now = s_state.open;
    if (open_now)
    {
      s_state.selected = 0;
      s_state.tab = Tab::System;
    }
  }

  // Escape pauses. The pause itself is handed to the worker thread -- doing it
  // on the host thread deadlocks against the compositor (see RequestCoreState).
  if (open_now)
  {
    std::lock_guard<std::mutex> lock(s_core_state_mutex);
    if (!s_settings_guard)
      s_settings_guard = std::make_unique<Config::ConfigChangeCallbackGuard>();
  }
  RequestCoreState(open_now);

  std::fprintf(stderr, "[menu] toggle open=%d (core %s)\n", open_now ? 1 : 0,
               open_now ? "pausing" : "resuming");
}

void CloseAndResume()
{
  {
    std::lock_guard<std::mutex> guard(s_state.mutex);
    if (!s_state.open)
      return;
    s_state.open = false;
  }
  RequestCoreState(false);   // resumes, then flushes the staged settings
}

void OnKey(Key key)
{
  Action action = Action::None;
  int slot = 1;
  std::function<void()> fullscreen_callback;
  std::function<void()> quit_callback;

  // Deferred work. NOTHING below may call into Config/Core/InputConfig while
  // the menu mutex is held: those can block on the CPU thread while the video
  // thread is blocked on this very mutex inside Draw(). Collect intent here,
  // act after unlocking.
  bool needs_config_save = false;
  bool needs_cheat_apply = false;
  bool enable_cheats_master = false;
  bool needs_build_controls = false;
  bool needs_load_cheats = false;
  ControllerEmu::Control* changed_pad_control = nullptr;
  std::vector<ActionReplay::ARCode> ar_snapshot;
  std::vector<Gecko::GeckoCode> gecko_snapshot;

  // Config::SetBase still happens under the mutex (AdjustItem) and a plain Set
  // fires OnConfigChanged -> callbacks synchronously. Declared out here so it
  // outlives the lock and callbacks defer until the mutex is long released.
  const Config::ConfigChangeCallbackGuard config_callback_guard;

  {
    std::lock_guard<std::mutex> guard(s_state.mutex);
    if (!s_state.open)
      return;

    // Ignore everything while waiting for a button press to bind.
    if (s_state.detector != nullptr)
      return;

    const int row_count = 1 + RowCount(s_state);  // row 0 = tab selector

    // Row 0: left/right switches tab, up/down moves into the list.
    if (s_state.selected == 0)
    {
      switch (key)
      {
      case Key::Left:
      case Key::Right:
      {
        const int dir = key == Key::Left ? -1 : 1;
        s_state.tab = static_cast<Tab>(
            (static_cast<int>(s_state.tab) + dir + kTabCount) % kTabCount);
        // These two build their rows on entry, but that means game-ini I/O and
        // InputConfig access -- deferred until the mutex is released.
        if (s_state.tab == Tab::Controls)
          needs_build_controls = true;
        else if (s_state.tab == Tab::Cheats)
          needs_load_cheats = true;
        break;
      }
      case Key::Up:
        s_state.selected = row_count - 1;
        break;
      case Key::Down:
        s_state.selected = row_count > 1 ? 1 : 0;
        break;
      case Key::Activate:
        break;
      }
    }
    else
    {
      const int index = s_state.selected - 1;
      switch (key)
      {
      case Key::Up:
        s_state.selected = (s_state.selected - 1 + row_count) % row_count;
        break;
      case Key::Down:
        s_state.selected = (s_state.selected + 1) % row_count;
        break;
      case Key::Left:
      case Key::Right:
      {
        const int dir = key == Key::Left ? -1 : 1;
        if (s_state.tab == Tab::Controls)
        {
          // Left clears a binding; the engine-side refresh happens after unlock.
          if (key == Key::Left && index < static_cast<int>(s_state.control_rows.size()))
          {
            auto* const control = s_state.control_rows[index].control;
            control->control_ref->SetExpression("");
            changed_pad_control = control;
          }
        }
        else if (s_state.tab != Tab::Cheats)
        {
          const auto& items = TabItems(s_state.tab);
          if (index < static_cast<int>(items.size()))
            needs_config_save = AdjustItem(items[index], dir, s_state);
        }
        break;
      }
      case Key::Activate:
        if (s_state.tab == Tab::Controls)
        {
          if (index < static_cast<int>(s_state.control_rows.size()))
            StartDetection(s_state, s_state.control_rows[index].control);
        }
        else if (s_state.tab == Tab::Cheats)
        {
          if (index == 0)
          {
            enable_cheats_master = !Config::Get(Config::MAIN_ENABLE_CHEATS);
          }
          else if (index - 1 < static_cast<int>(s_state.cheat_rows.size()))
          {
            const CheatRow& row = s_state.cheat_rows[index - 1];
            if (row.gecko)
              s_state.gecko_codes[row.index].enabled = !s_state.gecko_codes[row.index].enabled;
            else
              s_state.ar_codes[row.index].enabled = !s_state.ar_codes[row.index].enabled;
            enable_cheats_master = true;
          }
          ar_snapshot = s_state.ar_codes;
          gecko_snapshot = s_state.gecko_codes;
          needs_cheat_apply = true;
          needs_config_save = true;
        }
        else
        {
          const auto& items = TabItems(s_state.tab);
          if (index < static_cast<int>(items.size()))
            action = DecideAction(items[index], s_state, &needs_config_save);
        }
        break;
      }
    }

    slot = s_state.state_slot;
    fullscreen_callback = s_state.fullscreen_callback;
    quit_callback = s_state.quit_callback;
  }

  // Everything below runs with the mutex released, so these engine calls can
  // safely block without wedging the video thread inside Draw().
  if (needs_build_controls)
  {
    std::vector<ControlRow> built;
    BuildControlRowsData(&built);
    std::lock_guard<std::mutex> guard(s_state.mutex);
    s_state.control_rows = std::move(built);
  }

  if (needs_load_cheats)
  {
    std::vector<ActionReplay::ARCode> ar;
    std::vector<Gecko::GeckoCode> gecko;
    std::vector<CheatRow> cheat_rows;
    LoadCheatCodesData(&ar, &gecko, &cheat_rows);
    std::lock_guard<std::mutex> guard(s_state.mutex);
    s_state.ar_codes = std::move(ar);
    s_state.gecko_codes = std::move(gecko);
    s_state.cheat_rows = std::move(cheat_rows);
  }

  if (changed_pad_control != nullptr)
  {
    if (auto* const pad = GetPad())
    {
      pad->UpdateSingleControlReference(g_controller_interface,
                                        changed_pad_control->control_ref.get());
      if (auto* const config = Pad::GetConfig())
        config->SaveConfig();
    }
  }

  if (needs_cheat_apply)
  {
    Config::SetBase(Config::MAIN_ENABLE_CHEATS, enable_cheats_master);
    ApplyCheatCodes(ar_snapshot, gecko_snapshot);
  }

  if (needs_config_save)
    Config::Save();

  switch (action)
  {
  case Action::Fullscreen:
    if (fullscreen_callback)
      fullscreen_callback();
    break;
  case Action::Reset:
    // Close (and resume) first: ResetButton_Tap only schedules the reset, so it
    // lands once the CPU thread is running again rather than against a paused
    // core.
    CloseAndResume();
    Core::System::GetInstance().GetProcessorInterface().ResetButton_Tap();
    break;
  case Action::Quit:
    if (!quit_callback)
      break;
    if (Config::Get(RECOMP_AUTO_RESUME) &&
        Core::GetState(Core::System::GetInstance()) == Core::State::Running)
    {
      // Capture the auto-resume state before quitting. State::SaveAs only
      // QUEUES: the snapshot runs later on the CPU thread and the file is
      // written by the compress worker (temp file + rename), so quitting right
      // away could tear the core down first. A worker deletes the old file,
      // queues the save, waits for the renamed file to appear, then quits.
      // Never block the host thread on the CPU thread here (see the pause
      // deadlock note above).
      std::thread([quit_callback] {
        auto& system = Core::System::GetInstance();
        const std::string path = AutoResumePath();
        File::Delete(path);
        std::fprintf(stderr, "[autoresume] saving %s\n", path.c_str());
        ::State::SaveAs(system, path);
        for (int i = 0; i < 1000 && !File::Exists(path); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::fprintf(stderr, "[autoresume] save %s\n",
                     File::Exists(path) ? "complete" : "TIMED OUT");
        quit_callback();
      }).detach();
    }
    else
    {
      quit_callback();
    }
    break;
  case Action::SaveState:
  case Action::LoadState:
  {
    // Closing is cosmetic -- the core is never paused, so RunOnCPUThread always
    // sees a running CPU thread and the queued job actually executes.
    CloseAndResume();
    auto& system = Core::System::GetInstance();
    std::fprintf(stderr, "[menu] %s slot %d\n",
                 action == Action::SaveState ? "save" : "load", slot);
    if (action == Action::SaveState)
      ::State::Save(system, slot);
    else
      ::State::Load(system, slot);
    break;
  }
  case Action::None:
    break;
  }
}

void OnEscape()
{
  {
    std::lock_guard<std::mutex> guard(s_state.mutex);

    // While open, Escape unwinds one level at a time: cancel a pending bind,
    // then leave the subpage, and only then close. When closed, every path
    // below must fall through to Toggle so Escape still opens the menu.
    if (s_state.open)
    {
      if (s_state.detector != nullptr)
      {
        s_state.detector.reset();
        s_state.detecting_control = nullptr;
        return;
      }
      // Tabs are switched in place, so Escape always just closes the menu.
    }
  }

  Toggle();
}

void SetFullscreenCallback(std::function<void()> callback)
{
  std::lock_guard<std::mutex> guard(s_state.mutex);
  s_state.fullscreen_callback = std::move(callback);
}

void SetQuitCallback(std::function<void()> callback)
{
  std::lock_guard<std::mutex> guard(s_state.mutex);
  s_state.quit_callback = std::move(callback);
}

void SetFastForward(bool enable)
{
  // The override lives in the CurrentRun layer so releasing the key just
  // deletes it and the user's configured speed (base layer, Speed row) shows
  // through again — restoring a saved value here would instead mask any speed
  // change made while the key was down.
  static std::atomic<bool> s_active{false};
  if (s_active.exchange(enable) == enable)
    return;
  if (enable)
  {
    Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 0.0f);
    OSD::AddMessage("Fast-forward", 500);
  }
  else
  {
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_EMULATION_SPEED);
  }
}

void ScheduleAutoResumeLoad()
{
  if (!Config::Get(RECOMP_AUTO_RESUME))
    return;
  const std::string path = AutoResumePath();
  if (!File::Exists(path))
    return;
  // State::LoadAs needs a running CPU thread to execute the queued job, so
  // wait for Running off the host thread, plus a beat for the backends to
  // settle before swapping the whole machine state.
  std::thread([path] {
    auto& system = Core::System::GetInstance();
    for (int i = 0; i < 3000 && Core::GetState(system) != Core::State::Running; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (Core::GetState(system) != Core::State::Running)
    {
      std::fprintf(stderr, "[autoresume] core never reached Running; not loading\n");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    std::fprintf(stderr, "[autoresume] loading %s\n", path.c_str());
    ::State::LoadAs(system, path);
  }).detach();
}

void Draw()
{
  // The HUD is independent of the menu: it renders whenever enabled, and first
  // so an open menu draws over it.
  DrawTrainingHud();

  Tab tab;
  int selected;
  int state_slot;
  bool detecting;
  bool cheats_enabled = false;
  // label, value, highlight-value-green
  std::vector<std::tuple<std::string, std::string, bool>> rows;

  {
    std::lock_guard<std::mutex> guard(s_state.mutex);
    if (!s_state.open)
      return;

    tab = s_state.tab;
    selected = s_state.selected;
    state_slot = s_state.state_slot;
    detecting = s_state.detector != nullptr;

    switch (tab)
    {
    case Tab::Controls:
      for (const auto& row : s_state.control_rows)
      {
        std::string value = row.control->control_ref->GetExpression();
        if (value.empty())
          value = "-";
        rows.emplace_back(row.label, std::move(value), false);
      }
      break;
    case Tab::Cheats:
      cheats_enabled = Config::Get(Config::MAIN_ENABLE_CHEATS);
      rows.emplace_back("Enable Cheats", cheats_enabled ? "ON" : "OFF", cheats_enabled);
      for (const auto& row : s_state.cheat_rows)
      {
        const bool on = row.gecko ? s_state.gecko_codes[row.index].enabled
                                  : s_state.ar_codes[row.index].enabled;
        rows.emplace_back(row.label, on ? "ON" : "OFF", on);
      }
      break;
    default:
      for (const Item item : TabItems(tab))
        rows.emplace_back(ItemLabel(item), ItemValue(item, state_slot), false);
      break;
    }
  }

  const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
  const ImVec2 display = ImGui::GetIO().DisplaySize;

  ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(460.0f * scale, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.88f);

  if (ImGui::Begin("##recomp_menu", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoFocusOnAppearing))
  {
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "RING OUT  -  Ver 1.0");
    ImGui::Separator();

    // Tab bar. Selected row 0 means the tab strip itself has focus, so mark it
    // with arrows to show left/right will move between tabs.
    const bool tab_focused = selected == 0;
    for (int i = 0; i < kTabCount; ++i)
    {
      const Tab t = static_cast<Tab>(i);
      if (i != 0)
        ImGui::SameLine();

      if (t == tab)
      {
        const ImVec4 color = tab_focused ? ImVec4(1.0f, 1.0f, 0.45f, 1.0f) :
                                           ImVec4(1.0f, 0.85f, 0.4f, 1.0f);
        ImGui::TextColored(color, tab_focused ? "<%s>" : "[%s]", TabName(t));
      }
      else
      {
        ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 1.0f), " %s ", TabName(t));
      }
    }

    ImGui::Separator();
    ImGui::Spacing();

    const float value_column = ImGui::GetContentRegionAvail().x - 110.0f * scale;
    const float row_height = ImGui::GetTextLineHeightWithSpacing();
    const bool scrolling = rows.size() > 14;

    if (scrolling)
      ImGui::BeginChild("##rows", ImVec2(0.0f, row_height * 14.0f), false,
                        ImGuiWindowFlags_NoScrollbar);

    if (rows.empty())
    {
      ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "  (nothing here)");
      if (tab == Tab::Cheats)
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "  add codes to GameSettings/GRSEAF.ini");
    }

    for (size_t i = 0; i < rows.size(); ++i)
    {
      const bool is_selected = static_cast<int>(i) + 1 == selected;
      const ImVec4 color = is_selected ? ImVec4(1.0f, 1.0f, 0.45f, 1.0f) :
                                         ImVec4(0.85f, 0.85f, 0.85f, 1.0f);

      ImGui::TextColored(color, "%s %s", is_selected ? ">" : " ", std::get<0>(rows[i]).c_str());

      const std::string& value = std::get<1>(rows[i]);
      if (!value.empty())
      {
        ImGui::SameLine(value_column);
        if (is_selected && detecting)
          ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[press input]");
        else if (std::get<2>(rows[i]))
          ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", value.c_str());
        else
          ImGui::TextColored(color, "%s", value.c_str());
      }

      if (is_selected && scrolling)
        ImGui::SetScrollHereY(0.5f);
    }

    if (scrolling)
      ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();

    const char* hint = "Up/Down select   Left/Right change   Space confirm   Esc close";
    if (detecting)
      hint = "Press a key or button...   Esc cancel";
    else if (tab_focused)
      hint = "Left/Right switch tab   Down enter list   Esc close";
    else if (tab == Tab::Controls)
      hint = "Space rebind   Left clear   Up/Down select   Esc close";
    else if (tab == Tab::Cheats)
      hint = "Space toggle   Up/Down select   Esc close";
    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "%s", hint);
  }
  ImGui::End();
}

// Drives an in-progress input detection. Emulation is paused while the menu is
// open, so nothing else is polling the controller backends -- UpdateInput has
// to be called here or no input would ever be seen.
void UpdateDetection()
{
  ControllerEmu::Control* control = nullptr;
  ciface::Core::InputDetector::Results results;

  {
    std::lock_guard<std::mutex> guard(s_state.mutex);
    if (s_state.detector == nullptr)
      return;

    g_controller_interface.UpdateInput();
    s_state.detector->Update(std::chrono::seconds(3), std::chrono::milliseconds(0),
                             std::chrono::seconds(5));
    if (!s_state.detector->IsComplete())
      return;

    results = s_state.detector->TakeResults();
    control = s_state.detecting_control;
    s_state.detector.reset();
    s_state.detecting_control = nullptr;
  }

  // Applying the mapping touches InputConfig/ControllerInterface, so do it with
  // the menu lock released.
  if (control == nullptr || results.empty())
    return;

  auto* const pad = GetPad();
  if (pad == nullptr)
    return;

  ciface::MappingCommon::RemoveSpuriousTriggerCombinations(&results);
  const std::string expression = ciface::MappingCommon::BuildExpression(
      results, pad->GetDefaultDevice(), ciface::MappingCommon::Quote::On);

  control->control_ref->SetExpression(expression);
  pad->UpdateSingleControlReference(g_controller_interface, control->control_ref.get());
  if (auto* const config = Pad::GetConfig())
    config->SaveConfig();
}

// Debug aid: RECOMP_MENU_AUTOOPEN=<seconds> opens the menu by itself that long
// after the first tick, so the overlay can be verified without a keypress
// (synthetic input does not work on native Wayland).
void HostTick()
{
  // Free Look's camera is driven by its own input mapping (Shift+WASDQE to move,
  // Shift+mouse to look) and needs pumping from the host thread every iteration
  // -- DolphinQt does this from its HotkeyScheduler, which NoGUI has no
  // equivalent of. Device state itself stays fresh because SI polls
  // ControllerInterface on the CPU thread each frame.
  if (Config::Get(Config::FREE_LOOK_ENABLED))
    FreeLook::UpdateInput();

  static const int delay = [] {
    const char* const env = std::getenv("RECOMP_MENU_AUTOOPEN");
    return env != nullptr ? std::atoi(env) : 0;
  }();
  if (delay <= 0)
    return;

  static const auto start = std::chrono::steady_clock::now();
  static bool fired = false;
  if (fired || std::chrono::steady_clock::now() - start < std::chrono::seconds(delay))
    return;

  // RECOMP_MENU_STRESS=<n> hammers open/close n times to reproduce the
  // rapid-Escape deadlock without a human at the keyboard.
  if (const char* const stress = std::getenv("RECOMP_MENU_STRESS"))
  {
    fired = true;
    const int count = std::atoi(stress);
    std::fprintf(stderr, "[menu] stress: %d toggles\n", count);
    for (int i = 0; i < count; ++i)
    {
      OnEscape();
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      std::fprintf(stderr, "[menu] stress %d/%d open=%d\n", i + 1, count, IsOpen() ? 1 : 0);
    }
    std::fprintf(stderr, "[menu] stress complete, survived\n");
    return;
  }

  fired = true;
  std::fprintf(stderr, "[menu] auto-open firing\n");
  Toggle();

  // RECOMP_MENU_AUTOPAGE=cheats|controls jumps straight to a subpage, so those
  // lists can be verified without navigating.
  if (const char* const page = std::getenv("RECOMP_MENU_AUTOPAGE"))
  {
    // Loaders run before taking the lock -- same rule as OnKey.
    if (std::string(page) == "cheats")
    {
      std::vector<ActionReplay::ARCode> ar;
      std::vector<Gecko::GeckoCode> gecko;
      std::vector<CheatRow> cheat_rows;
      LoadCheatCodesData(&ar, &gecko, &cheat_rows);
      std::lock_guard<std::mutex> guard(s_state.mutex);
      s_state.ar_codes = std::move(ar);
      s_state.gecko_codes = std::move(gecko);
      s_state.cheat_rows = std::move(cheat_rows);
      s_state.tab = Tab::Cheats;
      std::fprintf(stderr, "[menu] auto-page cheats: %zu codes\n", s_state.cheat_rows.size());
    }
    else if (std::string(page) == "controls")
    {
      std::vector<ControlRow> built;
      BuildControlRowsData(&built);
      std::lock_guard<std::mutex> guard(s_state.mutex);
      s_state.control_rows = std::move(built);
      s_state.tab = Tab::Controls;
      std::fprintf(stderr, "[menu] auto-page controls: %zu rows\n", s_state.control_rows.size());
    }
  }
  std::fprintf(stderr, "[menu] auto-open done, open=%d\n", IsOpen() ? 1 : 0);
}

// The menu DOES pause emulation again (Escape = pause), but this must still
// never present.
//
// The original design paused the core and redrew the overlay by calling
// g_presenter->Present() from the host thread. That is unsound: Vulkan command
// buffers belong to the video thread, and "CPU paused" does not mean the video
// thread is idle -- it can still be draining the FIFO. Two threads then touch
// the backend at once. It cost three separate failures: a host-thread deadlock
// against the compositor, and a video thread wedged inside
// VKTexture::TransitionToLayout mid texture upload. Serialising Present alone
// could not fix it, because the collision is with the video thread's *other*
// GPU work, not with another Present.
//
// Pausing is safe now only because the two things that made it unsafe are gone:
//   * the pause itself runs on the CoreState worker, never the host thread, so
//     the compositor deadlock cannot form;
//   * settings are STAGED while paused and applied only after the resume (see
//     s_settings_guard), so no backend reconfigure ever lands on a paused core.
// Presenting from here would reintroduce the third failure, so it stays banned:
// this function still only services input detection, which touches no GPU state.
//
// KNOWN CONSEQUENCE: with the CPU paused the video thread stops producing
// frames, so the overlay is not repainted while the menu is open -- it shows
// whatever was on screen when the pause landed. Fixing that needs a redraw
// posted TO the video thread (AsyncRequests), never a Present from here.
void PumpFrame()
{
  if (!IsOpen())
    return;

  UpdateDetection();

  // With the core paused the CPU thread submits nothing, so the video thread
  // produces no frames and the overlay would sit frozen on whatever was on
  // screen when the pause landed. Fix it the only sound way: ask the VIDEO
  // thread to re-present, never present from here. RunGpuLoop pulls async
  // events BEFORE its "do nothing while paused" early-out, so the request is
  // serviced even while paused, and Presenter::Present() runs OnScreenUI's
  // Finalize/DrawImGui, which is what repaints the menu.
  //
  // Throttled to ~60 Hz: PumpFrame is called once per host-loop iteration,
  // which spins far faster than the display refresh.
  static std::chrono::steady_clock::time_point s_last_repaint{};
  const auto now = std::chrono::steady_clock::now();
  if (now - s_last_repaint < std::chrono::milliseconds(16))
    return;
  s_last_repaint = now;

  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_presenter)
      g_presenter->Present();
  });
}
}  // namespace RecompMenu
