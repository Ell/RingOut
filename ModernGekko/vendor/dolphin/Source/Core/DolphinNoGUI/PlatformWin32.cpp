// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include "Core/Config/ConfigManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/State.h"
#include "Core/System.h"

#include <Windows.h>
#include <chrono>
#include <climits>
#include <dwmapi.h>
#include <thread>

#include "VideoCommon/Present.h"
#include "VideoCommon/RecompMenu.h"
#include "VideoCommon/VideoConfig.h"
#include "resource.h"

namespace
{
class PlatformWin32 final : public Platform
{
public:
  ~PlatformWin32() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

private:
  static constexpr TCHAR WINDOW_CLASS_NAME[] = _T("DolphinNoGUI");

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  static bool RegisterRenderWindowClass();
  bool CreateRenderWindow();
  void ToggleFullscreen();
  bool HandleKeyDown(WPARAM key, LPARAM key_data);
  void UpdateWindowPosition();
  void ProcessEvents();

  HWND m_hwnd{};
  WINDOWPLACEMENT m_windowed_placement{};
  LONG_PTR m_windowed_style{};
  LONG_PTR m_windowed_ex_style{};

  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
};

PlatformWin32::~PlatformWin32()
{
  RecompMenu::SetFullscreenCallback({});
  RecompMenu::SetQuitCallback({});

  if (m_hwnd)
    DestroyWindow(m_hwnd);
}

bool PlatformWin32::RegisterRenderWindowClass()
{
  WNDCLASSEX wc = {};
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = 0;
  wc.lpfnWndProc = WndProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hIcon = LoadIcon(nullptr, IDI_ICON1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszMenuName = nullptr;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  wc.hIconSm = LoadIcon(nullptr, IDI_ICON1);

  if (!RegisterClassEx(&wc))
  {
    // Starting netplay tears down the offline Runtime, opens the lobby, then
    // constructs a second Runtime in the same process. Destroying the first
    // render window does not unregister its class, so rejecting the perfectly
    // valid existing registration makes every Windows netplay boot fail here.
    if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
      return true;

    MessageBox(nullptr, _T("Window registration failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  return true;
}

bool PlatformWin32::CreateRenderWindow()
{
  m_hwnd = CreateWindowEx(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, _T("Dolphin"), WS_OVERLAPPEDWINDOW,
                          m_window_x < 0 ? CW_USEDEFAULT : m_window_x,
                          m_window_y < 0 ? CW_USEDEFAULT : m_window_y, m_window_width,
                          m_window_height, nullptr, nullptr, GetModuleHandle(nullptr), this);
  if (!m_hwnd)
  {
    MessageBox(nullptr, _T("CreateWindowEx failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  ShowWindow(m_hwnd, SW_SHOW);
  UpdateWindow(m_hwnd);
  return true;
}

bool PlatformWin32::Init()
{
  if (!RegisterRenderWindowClass() || !CreateRenderWindow())
    return false;

  // Fullscreen and quit are window-system operations the overlay cannot perform
  // itself. Start Netplay also exits through the quit callback after writing
  // its restart request, so both callbacks are required for menu parity.
  RecompMenu::SetFullscreenCallback([this] { ToggleFullscreen(); });
  RecompMenu::SetQuitCallback([this] { Stop(); });

  if (Config::Get(Config::MAIN_FULLSCREEN))
    ToggleFullscreen();

  if (Config::Get(Config::MAIN_DISABLE_SCREENSAVER))
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

  UpdateWindowPosition();
  return true;
}

void PlatformWin32::SetTitle(const std::string& string)
{
  SetWindowTextW(m_hwnd, UTF8ToWString(string).c_str());
}

void PlatformWin32::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    UpdateWindowPosition();

    RecompMenu::HostTick();
    // Offline the menu pauses emulation, so keep overlay redraws moving from
    // the host loop. A netplay menu deliberately remains running and rides the
    // video thread's normal frames instead.
    if (RecompMenu::IsOpen() &&
        Core::GetState(Core::System::GetInstance()) == Core::State::Paused)
    {
      RecompMenu::PumpFrame();
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      continue;
    }

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Platform destruction happens after Runtime has shut Config down. Clear a
  // held Tab while Config is still alive instead of touching it in ~Platform.
  RecompMenu::SetFastForward(false);
}

WindowSystemInfo PlatformWin32::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Windows;
  wsi.render_window = reinterpret_cast<void*>(m_hwnd);
  wsi.render_surface = reinterpret_cast<void*>(m_hwnd);
  return wsi;
}

void PlatformWin32::ToggleFullscreen()
{
  if (!m_hwnd)
    return;

  if (!m_window_fullscreen)
  {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    if (!GetWindowPlacement(m_hwnd, &placement) || !GetMonitorInfo(monitor, &monitor_info))
    {
      return;
    }

    // Save the exact decorated/maximized state. Restoring it rather than
    // manufacturing a new WS_OVERLAPPEDWINDOW keeps Alt+Enter lossless.
    m_windowed_placement = placement;
    m_windowed_style = GetWindowLongPtr(m_hwnd, GWL_STYLE);
    m_windowed_ex_style = GetWindowLongPtr(m_hwnd, GWL_EXSTYLE);

    SetWindowLongPtr(m_hwnd, GWL_STYLE,
                     m_windowed_style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
    SetWindowLongPtr(m_hwnd, GWL_EXSTYLE,
                     m_windowed_ex_style & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE));
    const RECT& bounds = monitor_info.rcMonitor;
    SetWindowPos(m_hwnd, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left,
                 bounds.bottom - bounds.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    m_window_fullscreen = true;
    return;
  }

  SetWindowLongPtr(m_hwnd, GWL_STYLE, m_windowed_style);
  SetWindowLongPtr(m_hwnd, GWL_EXSTYLE, m_windowed_ex_style);
  SetWindowPlacement(m_hwnd, &m_windowed_placement);
  SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  m_window_fullscreen = false;
  UpdateWindowPosition();
}

bool PlatformWin32::HandleKeyDown(const WPARAM key, const LPARAM key_data)
{
  const bool alt =
      (key_data & (static_cast<LPARAM>(1) << 29)) != 0 || (GetKeyState(VK_MENU) & 0x8000) != 0;
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool repeat = (key_data & (static_cast<LPARAM>(1) << 30)) != 0;

  // Escape owns the overlay; Shift+Escape is the explicit immediate quit.
  // Ignore auto-repeat so holding Escape cannot open and immediately close it.
  if (key == VK_ESCAPE)
  {
    if (!repeat)
    {
      // If Tab was already held, do not carry unlimited speed through the
      // paused overlay or into the first frame after it closes.
      RecompMenu::SetFastForward(false);
      if (shift)
        RecompMenu::RequestQuit();
      else
        RecompMenu::OnEscape();
    }
    return true;
  }

  // These are Windows shell/window chords even while the overlay owns normal
  // navigation. DefWindowProc must see them so Alt+Tab can switch apps and
  // Alt+F4 can generate WM_CLOSE (which is routed through the safe quit path).
  if (alt && (key == VK_TAB || key == VK_F4))
    return false;

  // Once shutdown owns the resume-then-stop sequence, swallow every other
  // emulator hotkey so no late save/reset/pause action can race teardown.
  if (RecompMenu::IsQuitting())
    return true;

  if (RecompMenu::IsOpen())
  {
    // While open the overlay exclusively owns keyboard navigation. All other
    // keys are swallowed so they cannot leak through to the paused game.
    switch (key)
    {
    case VK_UP:
      RecompMenu::OnKey(RecompMenu::Key::Up);
      break;
    case VK_DOWN:
      RecompMenu::OnKey(RecompMenu::Key::Down);
      break;
    case VK_LEFT:
      RecompMenu::OnKey(RecompMenu::Key::Left);
      break;
    case VK_RIGHT:
      RecompMenu::OnKey(RecompMenu::Key::Right);
      break;
    case VK_SPACE:
    case VK_RETURN:
      // Navigation repeats are useful; repeating an activation can toggle a
      // cheat twice or launch an action more than once.
      if (!repeat)
        RecompMenu::OnKey(RecompMenu::Key::Activate);
      break;
    }
    return true;
  }

  if (key == VK_TAB && !alt)
  {
    // Hold Tab = unlimited speed. Alt+Tab remains a Windows shell shortcut.
    RecompMenu::SetFastForward(true);
    return true;
  }

  auto& system = Core::System::GetInstance();
  if (key == VK_F10)
  {
    if (!repeat)
      RecompMenu::TogglePause();
    return true;
  }
  // AltGr is reported as Ctrl+Alt. Do not turn ordinary text entry on those
  // keyboard layouts into a fullscreen or widescreen hotkey.
  if (key == VK_RETURN && alt && !ctrl)
  {
    if (!repeat)
      ToggleFullscreen();
    return true;
  }
  if (key == 'W' && alt && !ctrl)
  {
    if (!repeat)
    {
      const bool enable = !Config::Get(Config::GFX_WIDESCREEN_HACK);
      Config::SetBase(Config::GFX_WIDESCREEN_HACK, enable);
      Config::SetBase(Config::GFX_ASPECT_RATIO, enable ? AspectMode::ForceWide : AspectMode::Auto);
      Config::Save();
    }
    return true;
  }
  if (key >= VK_F1 && key <= VK_F8)
  {
    if (!repeat && !RecompMenu::IsNetplayActive())
    {
      const int slot = static_cast<int>(key - VK_F1 + 1);
      if (shift)
        State::Save(system, slot);
      else
        State::Load(system, slot);
    }
    return true;
  }
  if (key == VK_F9)
  {
    if (!repeat)
      Core::SaveScreenShot();
    return true;
  }
  if (key == VK_F11)
  {
    if (!repeat && !RecompMenu::IsNetplayActive())
      State::LoadLastSaved(system);
    return true;
  }
  if (key == VK_F12)
  {
    if (!repeat && !RecompMenu::IsNetplayActive())
    {
      if (shift)
        State::UndoLoadState(system);
      else
        State::UndoSaveState(system);
    }
    return true;
  }

  // Ordinary keys remain available to Windows and the controller backends.
  return false;
}

void PlatformWin32::UpdateWindowPosition()
{
  if (m_window_fullscreen)
    return;

  RECT rc = {};
  if (!GetWindowRect(m_hwnd, &rc))
    return;

  m_window_x = rc.left;
  m_window_y = rc.top;
  m_window_width = rc.right - rc.left;
  m_window_height = rc.bottom - rc.top;
}

void PlatformWin32::ProcessEvents()
{
  MSG msg;
  while (PeekMessage(&msg, m_hwnd, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

LRESULT PlatformWin32::WndProc(const HWND hwnd, const UINT msg, const WPARAM wParam,
                               const LPARAM lParam)
{
  PlatformWin32* platform = reinterpret_cast<PlatformWin32*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  switch (msg)
  {
  case WM_NCCREATE:
  {
    platform = static_cast<PlatformWin32*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(platform));
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  case WM_CREATE:
  {
    if (hwnd)
    {
      // Remove rounded corners from the render window on Windows 11
#if defined(__MINGW32__)
      // Ubuntu 24.04 ships mingw-w64 11, whose dwmapi.h predates these Windows 11
      // declarations. They are enum constants (not macros), so an #ifndef
      // fallback would also redeclare them with newer MinGW headers. The DWM API
      // takes DWORD values; 33 and 1 are the stable SDK values for
      // DWMWA_WINDOW_CORNER_PREFERENCE and DWMWCP_DONOTROUND respectively.
      constexpr DWORD corner_attribute = 33;
      constexpr DWORD corner_preference = 1;
#else
      constexpr DWORD corner_attribute = DWMWA_WINDOW_CORNER_PREFERENCE;
      constexpr DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_DONOTROUND;
#endif
      DwmSetWindowAttribute(hwnd, corner_attribute, &corner_preference, sizeof(corner_preference));
    }
  }
  break;

  case WM_SIZE:
  {
    if (g_presenter)
      g_presenter->ResizeSurface();
  }
  break;

  case WM_KEYDOWN:
  case WM_SYSKEYDOWN:
    if (platform && platform->HandleKeyDown(wParam, lParam))
      return 0;
    return DefWindowProc(hwnd, msg, wParam, lParam);

  case WM_KEYUP:
  case WM_SYSKEYUP:
    if (wParam == VK_TAB)
    {
      RecompMenu::SetFastForward(false);
      if (msg == WM_SYSKEYUP && (lParam & (static_cast<LPARAM>(1) << 29)) != 0)
        return DefWindowProc(hwnd, msg, wParam, lParam);
      return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);

  case WM_CHAR:
  case WM_SYSCHAR:
    // TranslateMessage queues character messages before WndProc sees the key
    // message. Consume overlay text and the Alt+Enter/Alt+W characters so
    // DefWindowProc does not beep or activate a system-menu mnemonic.
    if ((platform && RecompMenu::IsOpen()) ||
        (msg == WM_SYSCHAR && (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
         (wParam == VK_RETURN || wParam == 'w' || wParam == 'W')))
    {
      return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);

  case WM_SETFOCUS:
    if (platform)
      platform->m_window_focus = true;
    break;

  case WM_KILLFOCUS:
    if (platform)
      platform->m_window_focus = false;
    // A release is not delivered if focus changes while Tab is held.
    RecompMenu::SetFastForward(false);
    break;

  case WM_CLOSE:
    if (platform)
      RecompMenu::RequestQuit();
    break;

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  return 0;
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateWin32Platform()
{
  return std::make_unique<PlatformWin32>();
}
