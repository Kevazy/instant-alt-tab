#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace {

constexpr LONGLONG kMaxLogBytes = 1 << 20;  // rotated to <name>.old once larger

SRWLOCK g_lock = SRWLOCK_INIT;
std::wstring g_path;

void RotateIfLarge() {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &info)) return;
    LARGE_INTEGER size{.LowPart = info.nFileSizeLow, .HighPart = static_cast<LONG>(info.nFileSizeHigh)};
    if (size.QuadPart > kMaxLogBytes)
        MoveFileExW(g_path.c_str(), (g_path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace

void SetLogFile(std::wstring path) {
    AcquireSRWLockExclusive(&g_lock);
    g_path = std::move(path);
    ReleaseSRWLockExclusive(&g_lock);
}

void Log(const wchar_t* format, ...) {
    wchar_t message[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t line[600];
    int length = swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %s\r\n", now.wYear, now.wMonth, now.wDay,
                            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
    char utf8[sizeof(line) * 3 / sizeof(wchar_t)];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, length, utf8, sizeof(utf8), nullptr, nullptr);

    AcquireSRWLockExclusive(&g_lock);
    if (!g_path.empty()) {
        RotateIfLarge();
        HANDLE file =
            CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(file, utf8, static_cast<DWORD>(bytes), &written, nullptr);
            CloseHandle(file);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}
