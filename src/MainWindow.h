#pragma once

#include <windows.h>

bool RegisterMainWindowClass(HINSTANCE instance);
HWND CreateMainWindow(HINSTANCE instance, int showCommand);
