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

// Opens/closes the overlay. Pauses and resumes emulation as a side effect --
// with the core paused the video thread stops presenting, so while the menu is
// open the host loop must call PumpFrame() to keep it drawn and responsive.
void Toggle();

void OnKey(Key key);

// Escape: cancels an in-progress input detection, else backs out of a subpage,
// else closes the menu.
void OnEscape();

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

// Auto-resume, load half: if the feature is on and a save exists, waits (on a
// worker) for the core to reach Running and loads it. Call once after a
// successful boot. The save half lives in the menu's Quit action.
void ScheduleAutoResumeLoad();
}  // namespace RecompMenu
