#pragma once

// Small UTF-8 text files the agent leaves for the command-line tool (why
// Alt+Tab is stock, which builds the user was already told about).

#include <string>

std::wstring ReadTextFile(const std::wstring& path);  // empty if missing
bool WriteTextFile(const std::wstring& path, const std::wstring& text);
