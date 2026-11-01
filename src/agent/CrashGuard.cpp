#include "CrashGuard.h"

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace {

constexpr ULONGLONG kWindowMs = 60 * 60 * 1000;
constexpr size_t kCrashesToDisable = 2;

ULONGLONG NowMs() {
    FILETIME time;
    GetSystemTimeAsFileTime(&time);  // wall clock, so the record means the same after a reboot
    return ((static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime) / 10000;
}

}  // namespace

CrashGuard::CrashGuard(const std::wstring& directory) : m_path(directory + L"crash-guard.txt") {}

bool CrashGuard::Disabled() const { return Read().disabled; }

bool CrashGuard::RecordCrash() {
    Record record = Read();
    ULONGLONG now = NowMs();
    std::erase_if(record.crashes, [now](ULONGLONG time) { return now - time > kWindowMs; });
    record.crashes.push_back(now);
    record.disabled = record.crashes.size() >= kCrashesToDisable;
    Write(record);
    return record.disabled;
}

void CrashGuard::Clear() const { DeleteFileW(m_path.c_str()); }

// One crash time per line, then "disabled" once the guard has tripped.
CrashGuard::Record CrashGuard::Read() const {
    Record record;
    FILE* file = nullptr;
    if (_wfopen_s(&file, m_path.c_str(), L"r") != 0 || !file) return record;
    char line[64];
    while (fgets(line, sizeof(line), file)) {
        if (std::string_view(line).starts_with("disabled")) {
            record.disabled = true;
        } else if (ULONGLONG time = _strtoui64(line, nullptr, 10)) {
            record.crashes.push_back(time);
        }
    }
    fclose(file);
    return record;
}

void CrashGuard::Write(const Record& record) const {
    FILE* file = nullptr;
    if (_wfopen_s(&file, m_path.c_str(), L"w") != 0 || !file) return;
    for (ULONGLONG time : record.crashes) fprintf(file, "%llu\n", time);
    if (record.disabled) fprintf(file, "disabled\n");
    fclose(file);
}
