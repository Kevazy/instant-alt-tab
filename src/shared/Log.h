#pragma once

// A small append-only log file, shared by the agent and the hook DLL. Used for
// events worth seeing later (start, stop, rebuilds, failures), never per keystroke.

#include <string>

void SetLogFile(std::wstring path);
void Log(const wchar_t* format, ...);
