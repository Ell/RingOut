#include <cstdlib>
#include <cstdio>
#include "moderngekko/runtime.hpp"

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/HookableEvent.h"
#include "Common/MsgHandler.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Core.h"
#include "Core/HW/GBACore.h"
#include "Core/Host.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/RecompDeterminism.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/RecompMenu.h"
#include "VideoCommon/VideoConfig.h"
#include "dolphin_runtime_internal.hpp"
#include "moderngekko/cpu_state.h"
#include "moderngekko/module_loader.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fmt/format.h>
#include <mutex>
#include <thread>
#include <utility>

namespace {
static_assert(sizeof(ModernGekkoModuleDesc) == sizeof(StaticRecompModuleDesc));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_hashes) ==
              offsetof(StaticRecompModuleDesc, chunk_hashes));
std::mutex s_runtime_mutex;
bool s_runtime_active = false;
Platform *s_platform = nullptr;
std::string s_window_title;
bool s_show_fps_in_title = true;
bool s_external_ui_common = false;
std::unique_ptr<BootSessionData> s_boot_session_data;
u64 s_previous_net_wait_ns = 0;
double s_net_wait_ms_per_second = 0.0;
std::chrono::steady_clock::time_point s_previous_net_wait_sample;

#ifndef MODERNGEKKO_PROJECT_NAME
#define MODERNGEKKO_PROJECT_NAME "Ring Out"
#endif
#ifndef MODERNGEKKO_PROJECT_VERSION
#define MODERNGEKKO_PROJECT_VERSION "Ver 1.0"
#endif

std::string FormatWindowTitle(const std::string &title, double fps) {
  if (!std::isfinite(fps) || fps < 0.0)
    fps = 0.0;
  // Net-wait telemetry decoration removed: NetPlay::InputWaitTelemetry /
  // GetInputWaitTelemetry live only in an unpushed RecompCore fork. Single-
  // player title is just "<title> | <fps> FPS".
  (void)s_previous_net_wait_ns;
  (void)s_net_wait_ms_per_second;
  (void)s_previous_net_wait_sample;
  return fmt::format("{} | {:.1f} FPS", title, fps);
}
} // namespace

std::vector<std::string> Host_GetPreferredLocales() { return {}; }
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
bool Host_UIBlocksControllerState() { return false; }
void Host_Message(HostMessageID id) {
  if (id == HostMessageID::WMUserStop && s_platform)
    s_platform->Stop();
}
void Host_UpdateTitle(const std::string &) {
  if (!s_platform)
    return;

  auto &perf = Core::System::GetInstance().GetPerfMetrics();
  static const bool s_log_speed = std::getenv("STATICRECOMP_SPEED") != nullptr;
  if (s_log_speed)
    std::fprintf(stderr, "[perf] speed=%.1f%% fps=%.1f vps=%.1f\n",
                 perf.GetSpeed() * 100.0, perf.GetFPS(), perf.GetVPS());

  std::string title = s_window_title;
  if (s_show_fps_in_title &&
      s_platform->GetWindowSystemInfo().type != WindowSystemType::Headless)
    title = FormatWindowTitle(title, perf.GetFPS());
  s_platform->SetTitle(title);
}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus() {
  return !s_platform || s_platform->IsWindowFocused();
}
bool Host_RendererHasFullFocus() { return Host_RendererHasFocus(); }
bool Host_RendererIsFullscreen() {
  return s_platform && s_platform->IsWindowFullscreen();
}
bool Host_TASInputHasFocus() { return false; }
void Host_YieldToUI() {}
void Host_TitleChanged() {}
void Host_UpdateDiscordClientID(const std::string &) {}
bool Host_UpdateDiscordPresenceRaw(const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   std::int64_t, std::int64_t, int, int) {
  return false;
}
std::unique_ptr<GBAHostInterface>
Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) {
  return nullptr;
}

namespace moderngekko {
struct Runtime::Impl {
  RuntimeConfig config;
  GameMetadata metadata;
  std::string title;
  std::unique_ptr<Platform> platform;
  Common::EventHook state_hook;
  bool ui_initialized = false;
  bool controllers_initialized = false;
  bool booted = false;
  std::atomic<bool> running{false};
};

namespace detail {
void SetExternalUICommon(bool external) {
  std::lock_guard lock(s_runtime_mutex);
  s_external_ui_common = external;
}

void SetBootSessionData(std::unique_ptr<BootSessionData> boot_session_data) {
  std::lock_guard lock(s_runtime_mutex);
  s_boot_session_data = std::move(boot_session_data);
}
} // namespace detail

ModuleSource ModuleSource::DynamicPath(std::filesystem::path path) {
  ModuleSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModuleSource
ModuleSource::AttachedDescriptor(const ModernGekkoModuleDesc *descriptor) {
  ModuleSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  return source;
}

Runtime::Runtime(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

RuntimeCreateResult Runtime::Create(RuntimeConfig config) {
  std::lock_guard lock(s_runtime_mutex);
  if (s_runtime_active)
    return {
        {},
        RuntimeError{RuntimeErrorCode::AlreadyActive,
                     "only one ModernGekko runtime may be active per process"}};

  GameInspectResult inspected = InspectGame(config.game_root);
  if (!inspected)
    return {{}, RuntimeError{RuntimeErrorCode::InvalidGame, inspected.error}};

  const ModernGekkoModuleRequirements requirements = {
      MODERNGEKKO_CPU_ABI_VERSION, static_cast<std::uint32_t>(sizeof(CPUState)),
      inspected.metadata->disc_id.c_str()};
  ModuleLibrary validation_library;
  ModuleLoadResult module_result{};
  if (config.module.kind == ModuleSource::Kind::DynamicPath)
    module_result =
        validation_library.Open(config.module.path.string(), requirements);
  else if (config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    module_result =
        validation_library.Attach(config.module.descriptor, requirements);
  else if (!config.allow_interpreter)
    return {
        {},
        RuntimeError{
            RuntimeErrorCode::ModuleRequired,
            "no native module was supplied; use allow_interpreter explicitly"}};

  if (config.module.kind != ModuleSource::Kind::None &&
      module_result.status != ModuleLoadStatus::Ok) {
    if (!config.allow_interpreter) {
      std::string message = "native module was rejected";
      if (module_result.status == ModuleLoadStatus::DescriptorRejected)
        message += ": " + std::string(moderngekko_module_status_string(
                              module_result.validation_status));
      return {
          {},
          RuntimeError{RuntimeErrorCode::ModuleRejected, std::move(message)}};
    }
    config.module = {};
  }
  validation_library.Close();

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->metadata = std::move(*inspected.metadata);
  impl->title = impl->config.window_title.value_or(
      std::string(MODERNGEKKO_PROJECT_NAME) + " " + MODERNGEKKO_PROJECT_VERSION);

  if (!s_external_ui_common) {
    UICommon::SetUserDirectory(impl->config.user_directory.string());
    // Only DolphinQt's main() called this, so running headless/NoGUI left the
    // user directory without StateSaves/, Screenshots/, Logs/, Maps/ etc. and
    // anything writing there failed with "failed to create file" -- savestates
    // in particular.
    UICommon::CreateDirectories();
    UICommon::Init();
    // Dolphin's default non-Windows alert handler answers "No" to every
    // question, and ASSERT's PanicYesNo treats "No" as "don't ignore" ->
    // Crash(). A GFX FIFO hiccup then kills the whole game (seen live: dual
    // core desync mid-session, SIGILL). There is no UI to ask, so log the
    // alert and always pick the continue path.
    Common::RegisterMsgAlertHandler(
        [](const char* caption, const char* text, bool yes_no, Common::MsgType style) -> bool {
          std::fprintf(stderr, "[alert] %s: %s\n", caption, text);
          return true;
        });
    impl->ui_initialized = true;
  }

  if (impl->config.headless)
    impl->platform = Platform::CreateHeadlessPlatform();
#ifdef _WIN32
  else
    impl->platform = Platform::CreateWin32Platform();
#endif
#ifdef MODERNGEKKO_HAVE_COCOA
  else impl->platform = Platform::CreateMacOSPlatform();
#endif
#ifdef HAVE_X11
  else if (impl->config.window_system != WindowSystem::Wayland) impl->platform =
      Platform::CreateX11Platform();
#endif
#ifdef HAVE_WAYLAND
  else if (impl->config.window_system != WindowSystem::X11) impl->platform =
      Platform::CreateWaylandPlatform();
#endif
  if (!impl->platform || !impl->platform->Init()) {
    if (impl->ui_initialized)
      UICommon::Shutdown();
    return {{},
            RuntimeError{RuntimeErrorCode::PlatformUnavailable,
                         "the requested Dolphin host platform is unavailable"}};
  }

  const WindowSystemInfo wsi = impl->platform->GetWindowSystemInfo();
  UICommon::InitControllers(wsi);
  impl->controllers_initialized = true;
  impl->platform->SetTitle(impl->title);

  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  // Dolphin defaults CPUThread=false (single-core) on desktop, which runs the
  // GPU synchronously on the CPU thread — every full-screen XFB blit / texture
  // upload (heavy during FMV) then stalls the recompiled core. Dual-core moves
  // the GPU to its own thread (matching real GC's async GP), the biggest perf
  // win for FMV/gameplay. The recomp core drives the FIFO like any CPU core, so
  // this is orthogonal to StaticRecomp.
  // ...except when hashing state per frame. Dual-core has the GPU thread
  // writing guest RAM asynchronously, so RAM read at a CPU frame boundary is
  // racy by construction and two runs would differ whether or not the core is
  // deterministic. The determinism harness therefore needs single-core, or it
  // measures its own noise.
  //
  // RINGOUT_DETERMINISM_DUALCORE=1 lifts that, so the harness can measure the
  // configuration netplay actually ships (dual-core + a deterministic GPU
  // thread) instead of one it does not. It is only sound alongside the quiesce
  // in RecompDeterminism::OnFrame -- without that the hashes race and the run
  // measures its own noise, which is the trap this whole comment is about. Left
  // opt-in so every previously verified result keeps its exact shape.
  const bool determinism_dual_core =
      RecompDeterminism::IsActive() && std::getenv("RINGOUT_DETERMINISM_DUALCORE") != nullptr;
  Config::SetBase(Config::MAIN_CPU_THREAD,
                  !RecompDeterminism::IsActive() || determinism_dual_core);
  if (determinism_dual_core)
    Config::SetBase(Config::MAIN_GPU_DETERMINISM_MODE, std::string("fake-completion"));
  if (RecompDeterminism::IsActive()) {
    // Pin the clock the same way NetPlayServer does (NetPlayServer.cpp:2088):
    // the RTC is converted to timebase ticks at boot, so two runs started
    // seconds apart diverge from frame 0 through every value the game seeds
    // from it. Forced here rather than left to the ini, because the setting is
    // spelled EnableCustomRTC and getting that wrong fails silently -- which is
    // exactly what happened to the first attempt at this measurement.
    Config::SetBase(Config::MAIN_CUSTOM_RTC_ENABLE, true);
    Config::SetBase(Config::MAIN_CUSTOM_RTC_VALUE, 0x386D4380u);
  }
  // SoulCalibur II (GRSEAF): the OS scheduler spins in an idle loop at
  // 0x80185DEC waiting for an interrupt to wake a task. Without idle-skip the
  // recomp core burns real wall-time executing that spin, which halved FMV /
  // gameplay speed (movies ran in slow-motion). Pointing the core's idle-skip
  // at that PC makes CoreTiming fast-forward to the next event instead → full
  // 60fps. (Idle-skip is the standard Dolphin approach; only the PC is game-
  // specific, so it is scoped to this disc ID.)
  // GRSEPS is the "SC2 Plus" community mod. It appends its own code at
  // 0x80476000 and hooks the base text in place rather than relocating it, so
  // the scheduler and its idle loop stay where they were: the four
  // instructions at 0x80185DEC are byte-identical between the two discs
  // (verified section by section against the stock DOL). Same spin, same skip.
  if (impl->metadata.disc_id == "GRSEAF" || impl->metadata.disc_id == "GRSEPS")
    Config::SetBase(Config::MAIN_STATICRECOMP_IDLE_PC, 0x80185DECu);
  if (!impl->config.graphics.backend.empty())
    Config::SetBase(Config::MAIN_GFX_BACKEND, impl->config.graphics.backend);
#ifdef _WIN32
  // Default to Direct3D on Windows. Vulkan is only present if the GPU driver
  // installed vulkan-1.dll, and on a machine without it the failure is fatal
  // and opaque -- "Failed to load Vulkan library", then "Failed to initialize
  // video backend!", and the emulated CPU never starts (native=0). D3D11 ships
  // with Windows itself, so it always works. This is the BASE layer, so a
  // backend chosen in the settings menu still wins.
  else if (!impl->config.headless)
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("D3D"));
#endif
  else if (impl->config.headless)
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Null"));
  if (impl->config.graphics.internal_resolution_scale)
    Config::SetBase(Config::GFX_EFB_SCALE,
                    *impl->config.graphics.internal_resolution_scale);
  // SoulCalibur II and most GC titles render a 4:3 projection. ForceWide alone
  // would just stretch that image; the widescreen hack widens the projection
  // matrix so the extra horizontal field of view is actually drawn. The pair is
  // set together — either both on (16:9) or both off (native 4:3). Alt+W flips
  // them at runtime via Config::SetCurrent (VideoConfig::Refresh picks it up on
  // the next frame).
  // Only forced when --widescreen is passed; otherwise the value saved by the
  // in-game menu (Alt+W / Settings) carries over between launches.
  if (impl->config.graphics.widescreen) {
    Config::SetBase(Config::GFX_WIDESCREEN_HACK, *impl->config.graphics.widescreen);
    Config::SetBase(Config::GFX_ASPECT_RATIO, *impl->config.graphics.widescreen
                                                  ? AspectMode::ForceWide
                                                  : AspectMode::Auto);
  }
  Config::SetBase(Config::GFX_SHADER_CACHE, true);
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  ShaderCompilationMode::AsynchronousUberShaders);
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);
  const std::vector<std::string> audio_backends =
      AudioCommon::GetSoundBackends();
  if (impl->config.headless) {
    impl->config.audio.backend = BACKEND_NULLSOUND;
  } else if (impl->config.audio.backend.empty() ||
             std::ranges::find(audio_backends, impl->config.audio.backend) ==
                 audio_backends.end()) {
    impl->config.audio.backend = AudioCommon::GetDefaultSoundBackend();
    if (impl->config.audio.backend == BACKEND_NULLSOUND) {
      const auto available =
          std::ranges::find_if(audio_backends, [](const std::string &backend) {
            return backend != BACKEND_NULLSOUND;
          });
      if (available != audio_backends.end())
        impl->config.audio.backend = *available;
    }
  }
  Config::SetBase(Config::MAIN_AUDIO_BACKEND, impl->config.audio.backend);
  Config::SetBase(Config::MAIN_INPUT_BACKGROUND_INPUT,
                  impl->config.input.background_input);

  auto &jit = Core::System::GetInstance().GetJitInterface();
  if (impl->config.module.kind == ModuleSource::Kind::DynamicPath)
    jit.SetStaticRecompModuleSource(
        StaticRecompModuleSource::Dynamic(impl->config.module.path.string()));
  else if (impl->config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    jit.SetStaticRecompModuleSource(StaticRecompModuleSource::Attached(
        reinterpret_cast<const StaticRecompModuleDesc *>(
            impl->config.module.descriptor)));
  else
    jit.SetStaticRecompModuleSource({});

  s_runtime_active = true;
  s_platform = impl->platform.get();
  s_window_title = impl->title;
  s_show_fps_in_title = impl->config.show_fps_in_title;
  return {std::unique_ptr<Runtime>(new Runtime(std::move(impl))), {}};
}

Runtime::~Runtime() {
  RequestStop();
  if (m_impl->booted) {
    Core::Stop(Core::System::GetInstance());
    Core::Shutdown(Core::System::GetInstance());
  }
  m_impl->state_hook = {};
  if (m_impl->controllers_initialized)
    UICommon::ShutdownControllers();
  if (m_impl->ui_initialized)
    UICommon::Shutdown();
  std::lock_guard lock(s_runtime_mutex);
  s_platform = nullptr;
  s_window_title.clear();
  s_show_fps_in_title = true;
  s_runtime_active = false;
}

RuntimeRunResult Runtime::Run() {
  if (m_impl->running.exchange(true))
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::InvalidState,
                         "runtime is already running"}};

  std::unique_ptr<BootParameters> boot;
  {
    std::lock_guard lock(s_runtime_mutex);
    if (s_boot_session_data)
      boot = BootParameters::GenerateFromFile(
          m_impl->metadata.main_dol.string(), std::move(*s_boot_session_data));
    else
      boot =
          BootParameters::GenerateFromFile(m_impl->metadata.main_dol.string());
    s_boot_session_data.reset();
  }
  if (!boot) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin rejected the extracted disc"}};
  }
  m_impl->state_hook =
      Core::AddOnStateChangedCallback([this](Core::State state) {
        if (state == Core::State::Uninitialized && m_impl->platform)
          m_impl->platform->Stop();
      });
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot),
                             m_impl->platform->GetWindowSystemInfo())) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin could not boot sys/main.dol"}};
  }
  m_impl->booted = true;
  // Continue-where-you-left-off (menu System > Auto-Resume). Headless runs are
  // benchmarks/verification; loading a state there would corrupt them.
  if (!m_impl->config.headless)
    RecompMenu::ScheduleAutoResumeLoad();
  std::jthread title_thread;
  if (!m_impl->config.headless && m_impl->config.show_fps_in_title) {
    title_thread = std::jthread([](std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        Host_UpdateTitle({});
        for (int i = 0; i < 10 && !stop_token.stop_requested(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  m_impl->platform->MainLoop();
  title_thread.request_stop();
  if (title_thread.joinable())
    title_thread.join();
  m_impl->platform->SaveWindowGeometry();
  Core::Stop(Core::System::GetInstance());
  Core::Shutdown(Core::System::GetInstance());
  m_impl->booted = false;
  m_impl->running = false;
  return {};
}

void Runtime::RequestStop() {
  if (m_impl && m_impl->platform)
    m_impl->platform->RequestShutdown();
}

std::optional<RuntimeError> Runtime::Pause() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Paused);
  return {};
}

std::optional<RuntimeError> Runtime::Resume() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  return {};
}

const RuntimeConfig &Runtime::GetConfig() const { return m_impl->config; }
const GameMetadata &Runtime::GetGameMetadata() const {
  return m_impl->metadata;
}
const std::string &Runtime::GetWindowTitle() const { return m_impl->title; }
} // namespace moderngekko
