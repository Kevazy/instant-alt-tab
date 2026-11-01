#pragma once

// Turns Instant Alt+Tab off if Explorer keeps crashing while the hooks are
// active: two crashes within an hour disable it until the next install. Only
// crashes count (Explorer ending with an error status), not restarts or
// sign-out. The record is a small text file, so it survives agent restarts.

#include <windows.h>

#include <string>
#include <vector>

class CrashGuard {
public:
    explicit CrashGuard(const std::wstring& directory);

    // An error status such as 0xC0000005, or 0xE06D7363 for an unhandled C++
    // exception: what a crash exits with. Not 0xFFFFFFFF (-1), which Explorer
    // exits with when it's restarted.
    static bool IsCrash(DWORD exitCode) { return exitCode >= 0xC0000000 && exitCode != 0xFFFFFFFF; }

    bool Disabled() const;
    // Returns true if this crash disabled Instant Alt+Tab.
    bool RecordCrash();
    void Clear() const;

private:
    struct Record {
        std::vector<ULONGLONG> crashes;  // wall-clock milliseconds
        bool disabled = false;
    };
    Record Read() const;
    void Write(const Record& record) const;

    std::wstring m_path;
};
