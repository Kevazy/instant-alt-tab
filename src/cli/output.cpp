#include "output.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace {

void Write(const wchar_t* format, va_list args, bool endLine) {
    wchar_t text[1024];
    _vsnwprintf_s(text, _TRUNCATE, format, args);
    std::wstring line = std::wstring(text) + (endLine ? L"\n" : L"");

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, written;
    if (GetConsoleMode(output, &mode)) {
        WriteConsoleW(output, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        return;
    }
    int size = static_cast<int>(line.size());
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), size, nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), size, utf8.data(), bytes, nullptr, nullptr);
    WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

}  // namespace

void Print(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    Write(format, args, true);
    va_end(args);
}

void Ask(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    Write(format, args, false);
    va_end(args);
}
