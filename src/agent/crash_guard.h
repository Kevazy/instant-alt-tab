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

    // An NTSTATUS error code such as 0xC0000005, which is what a crash exits with.
    // Not 0xFFFFFFFF (-1), which Explorer exits with when it's restarted.
    static bool IsCrash(DWORD exitCode) { return (exitCode & 0xF0000000) == 0xC0000000; }

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

    std::wstring path_;
};
