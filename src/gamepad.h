// AKVR — gamepad capture + ImGui navigation feed (interactive settings menu)
// ------------------------------------------------------------------
// We want the physical controller to drive the settings panel WITHOUT the game
// also reacting (Batman jumping/moving while you nudge a slider). To do that we
// intercept the game's controller reads (XInputGetState) and, while "menu mode"
// is on, hand the game a neutral/empty controller — while we separately read the
// REAL controller (through the un-intercepted original) and feed it to the menu.
// ------------------------------------------------------------------
#pragma once

// Install the XInputGetState interception. Lazy + idempotent — call each frame
// until it succeeds (the game may load its input DLL after us).
void akvr_gamepad_install();

// Menu mode: when ON, the game sees an empty controller (so it ignores the pad
// while you're in the settings menu); the menu gets the real controller instead.
void akvr_gamepad_set_menu(bool on);
bool akvr_gamepad_menu();

// Edge-detected "Menu + View held together" (Start + Back) from the REAL controller
// — used to open/close menu mode. Works even while the game is being given an empty
// pad, because it reads the original (un-intercepted) controller state.
bool akvr_gamepad_menu_chord_pressed();

// Diagnostics: what we are actually doing to the game's view of the pad. If the game
// stops responding to the controller, this says whether WE are starving it (blanking)
// or whether the pad isn't reaching us either (connected=false / buttons stuck at 0).
struct GamepadDiag
{
    bool           hooked;      // our XInput detour is installed
    bool           connected;   // the real pad reads OK
    bool           blanking;    // we are zeroing the pad for the game right now
    unsigned short buttons;     // raw button bits from the REAL pad
    short          lx, ly;      // raw left stick
};
GamepadDiag akvr_gamepad_diag();

// Feed the real controller into ImGui as navigation input. Call once per frame,
// after ImGui_ImplWin32_NewFrame() and before ImGui::NewFrame(), only useful in
// menu mode (no-op otherwise).
void akvr_gamepad_feed_imgui(bool muteLeftRight = false);   // ONESTEP: left/right held back while a slider owns them
