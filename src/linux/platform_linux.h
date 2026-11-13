// platform_linux.h (Linux build only)
//   What the Linux build needs from the system beyond the Windows stand-ins (win32_compat.h):
//   the game's X11 window and its input, fonts, the clipboard.
#pragma once

#include <string>
#include <vector>

namespace mxcam {
namespace platform {

// ---- The game's window (input_linux.cpp) ----
// Seen as the game draws: its display (an X11 Display*) and the window it draws into (an X11
// Window id). The first glXSwapBuffers / glXMakeCurrent / XCreateWindow names them.
void NoteGameWindow(void* display, unsigned long window);
void* GameDisplay();
unsigned long GameWindow();
// The keyboard and the mouse as they are now (a Windows virtual-key code: VK_LBUTTON ... 0xFE).
bool KeyDown(int vk);
// The pointer over the game's window (pixels from its top left), and the window's size.
bool PointerInWindow(int& x, int& y);
bool WindowSize(int& width, int& height);
bool WindowFocused();

// ---- Fonts (fonts_linux.cpp) ----
// The bytes of a TrueType/OpenType file for a face like the Windows one ("Arial", "Segoe UI") at
// a weight (400 regular, 600 semibold, 700 bold), as fontconfig matches it; empty if none.
const std::vector<unsigned char>& FontFile(const char* face, int weight);

// ---- The clipboard (input_linux.cpp): X11 or Wayland, through the usual tools ----
bool ClipboardSet(const std::string& text);
std::string ClipboardGet();

}  // namespace platform
}  // namespace mxcam
