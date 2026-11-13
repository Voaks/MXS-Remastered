// overlay.h
//   In-game settings panel (Config::menuKey, E by default).
#pragma once

#include <windows.h>

namespace mxcam {

// Polls the menu key and, while the panel is open, handles its input and draws it into the
// current back buffer. Called by the swap hook right before the real buffer swap.
void OverlayFrame(HDC dc);

// True while a KEYBINDS row waits for its new key (the hotkeys stay quiet meanwhile).
bool OverlayListeningForKey();

// True while the settings panel is open (the controller is read then, for binding its looks).
bool OverlayPanelOpen();

// Hiding the game's HUD and cursor while riding (OTHER tab: Config::hideHud / hideCursor).
// They're hidden while the 3D scene is on screen (riding, spectating, a replay; never the game's
// menus) and the game's menu isn't open over it (GameMenuOverScene). The HUD goes at once; the
// cursor once the mouse is left alone (moving or clicking it shows it for a few seconds).
// cursor: asks about the cursor, else the HUD.
bool HideGameOverlayNow(bool cursor);

// Esc presses that went to the game since last asked (it opens and closes the game's menu).
int TakeGameEscapes();

// The mouse in the game window's client area (pixels from its top left), false if unknown.
bool GameCursorPos(float& x, float& y);

// The game's input as it arrives (Linux, from its X11 events): whether the menu keeps presses to
// itself (true while it's open), the mouse wheel (notches, + away from you), and a key going down
// for the game (Esc: the game's menu; see HideGameOverlayNow).
bool OverlayTakesInput();
void OverlayWheel(int notches);
void OverlayGameKeyDown(int vk, bool repeat);

}  // namespace mxcam
