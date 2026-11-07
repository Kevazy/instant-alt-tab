#include "Console.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

// The console's colour sequences are switched on for as long as this process
// runs, and the console is left as it was found.
class Terminal {
public:
    Terminal() : output_(GetStdHandle(STD_OUTPUT_HANDLE)) {
        sequences_ =
            GetConsoleMode(output_, &mode_) && SetConsoleMode(output_, mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        // NO_COLOR (no-color.org): set to anything but an empty string, it turns colours off.
        colors_ = sequences_ && GetEnvironmentVariableW(L"NO_COLOR", nullptr, 0) <= 1;
    }
    ~Terminal() {
        if (sequences_) SetConsoleMode(output_, mode_);
    }
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    bool Sequences() const { return sequences_; }
    bool Colors() const { return colors_; }

private:
    HANDLE output_;
    DWORD mode_ = 0;
    bool sequences_ = false, colors_ = false;
};

Terminal& TheTerminal() {
    static Terminal terminal;
    return terminal;
}

// Replaces the style marks in a format with colour sequences, or leaves them out.
// Bright colours, made for the dark backgrounds consoles open with (and
// PowerShell 5's blue one): aqua for success, yellow for warnings, red for
// failures. `</>` resets only the colour and the weight, so a console's own
// background stays.
std::wstring ApplyStyles(std::wstring_view format) {
    struct Style {
        std::wstring_view mark, sequence;
    };
    static constexpr Style kStyles[] = {{L"<b>", L"\x1b[1m"},    {L"<good>", L"\x1b[96m"}, {L"<warn>", L"\x1b[93m"},
                                        {L"<bad>", L"\x1b[91m"}, {L"<dim>", L"\x1b[90m"},  {L"</>", L"\x1b[22;39m"}};
    bool colors = TheTerminal().Colors();
    std::wstring styled;
    while (!format.empty()) {
        const Style* found = nullptr;
        if (format.front() == L'<') {
            for (const Style& style : kStyles) {
                if (format.starts_with(style.mark)) found = &style;
            }
        }
        if (found) {
            if (colors) styled += found->sequence;
            format.remove_prefix(found->mark.size());
        } else {
            styled += format.front();
            format.remove_prefix(1);
        }
    }
    return styled;
}

void Write(const wchar_t* format, va_list args, bool endLine) {
    wchar_t text[2048];
    _vsnwprintf_s(text, _TRUNCATE, ApplyStyles(format).c_str(), args);
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

int ConsoleColumns() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return 0;
    return info.srWindow.Right - info.srWindow.Left + 1;
}

bool ClearLine() {
    if (!TheTerminal().Sequences()) return false;
    constexpr std::wstring_view kClear = L"\r\x1b[2K";  // to the line's start, then clear it
    DWORD written;
    return WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), kClear.data(), static_cast<DWORD>(kClear.size()), &written,
                         nullptr);
}

void ClearScreen() {
    // Redirected, or a legacy console that would print the sequence as text: leave it.
    if (!TheTerminal().Sequences()) return;
    constexpr std::wstring_view kClear = L"\x1b[2J\x1b[3J\x1b[H";  // the screen, the scrollback, cursor to the top
    DWORD written;
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), kClear.data(), static_cast<DWORD>(kClear.size()), &written, nullptr);
}
