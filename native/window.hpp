#pragma once
#include <windows.h>

namespace ark {
// Attach only from the thread that owns the window.
bool attachWindowResize(HWND window);
bool startWindowResize(HMODULE module);
void stopWindowResize();
}
