// Copyright 2026 ModernGekko Project
// SPDX-License-Identifier: GPL-2.0-or-later

// In-game settings overlay for the static-recompilation runtime, in the style of
// other console recompilation projects: Escape opens a pause menu that exposes
// the settings a player actually wants mid-session (widescreen, internal
// resolution, volume, save states) without dropping to a config file.
//
// Threading: OnKey/Toggle are called from the host (platform) thread, Draw from
// the video thread inside OnScreenUI::Finalize. All state is behind a mutex.

#pragma once

#include <functional>
#include <optional>
#include <string>

namespace RecompMenu
{
enum class Key
{
  Up,
  Down,
  Left,
  Right,
  Activate,
};

bool IsOpen();

// Lock-free CPU/input-thread view. Usually matches IsOpen(), but can remain
// true briefly after a controller closes the overlay so that close button is
// not delivered to the game before the player releases it.
bool CapturesGameInput();

// True after Dolphin has armed an active deterministic netplay session. Kept
// here so platform backends do not need to include NetPlayProto.h (whose enum
// names collide with Xlib's legacy macros).
bool IsNetplayActive();

// Lock-free terminal-state view. Once true, input paths must not queue another
// pause/menu action while the tracked resume-then-stop task owns shutdown.
bool IsQuitting();

// Opens/closes the overlay. Pauses and resumes emulation as a side effect --
// with the core paused the video thread stops presenting, so while the menu is
// open the host loop must call PumpFrame() to keep it drawn and responsive.
void Toggle();

void OnKey(Key key);

// Escape: cancels an in-progress input detection, else backs out of a subpage,
// else closes the menu.
void OnEscape();

// Requests an immediate quit without racing shutdown against a CPU thread that
// the overlay (or F10) has paused. Platforms should route window-close and
// Shift+Escape here instead of calling their shutdown callback directly.
// Returns true when this call started a quit or one is already in flight. A
// false result means the current platform did not install an overlay quit
// callback, so callers may fall back to the platform's direct shutdown path.
bool RequestQuit();

// F10 pause/resume, handed to the same off-host-thread state worker used by the
// overlay so a platform event loop never blocks waiting for the CPU thread.
// Returns the requested paused state; active netplay refuses the request and
// returns false because pausing only one peer would stall/desync the session.
bool TogglePause();

// Closes transient UI state and waits for the pause worker/config guard to
// settle. Runtime calls this after the platform loop and before Core/Config
// teardown, including disconnect and signal-driven exits.
void PrepareForShutdown();

// Builds the ImGui window. Called from OnScreenUI::Finalize with the ImGui lock
// held; does nothing when closed.
void Draw();

// Called once per host-loop iteration, menu open or not. Currently only drives
// the RECOMP_MENU_AUTOOPEN debug aid.
void HostTick();

// Forces a redraw while emulation is paused. Safe only while the menu is open
// (the video thread is idle then). No-op otherwise.
void PumpFrame();

// Actions the menu cannot perform itself -- supplied by the platform layer.
void SetFullscreenCallback(std::function<void()> callback);
void SetQuitCallback(std::function<void()> callback);

// Hold-to-fast-forward. true overrides MAIN_EMULATION_SPEED with 0 (unlimited)
// in the session layer; false removes the override so the configured speed is
// back in charge. Idempotent, callable from the host thread.
void SetFastForward(bool enable);

// Auto-resume, load half: returns the offline snapshot that should be supplied
// to Dolphin's native BootSessionData path. Loading during boot runs on the CPU
// thread and avoids blocking the host event loop. A netplay BootSessionData
// takes precedence in Runtime, so a local snapshot can never enter netplay.
std::optional<std::string> GetAutoResumePathForBoot();
}  // namespace RecompMenu
