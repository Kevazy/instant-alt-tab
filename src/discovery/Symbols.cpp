#include "Symbols.h"

#include <windows.h>
#include <dbghelp.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

namespace Discovery {

namespace {

constexpr wchar_t kSymbolServer[] = L"msdl.microsoft.com";
constexpr uint64_t kMaxPdbBytes = 512ull << 20;  // far above any shell PDB; guards against a runaway response

std::wstring SignatureKey(const PeFile::DebugInfo& info) {
    // The symbol server's key: the GUID's 32 hex digits, then the age in hex.
    wchar_t key[48];
    const GUID& g = info.guid;
    swprintf_s(key, L"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X", g.Data1, g.Data2, g.Data3, g.Data4[0],
               g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], info.age);
    return key;
}

// The cache holds a line per requested name: "<rva in hex>,<rva>... <name>", "-" for none.
// Anything unreadable makes it a miss, so a damaged file is simply rebuilt.
std::optional<std::map<std::wstring, std::vector<uint32_t>>> ReadCache(const std::wstring& path,
                                                                       const std::vector<std::wstring>& names) {
    std::wifstream stream(path);
    if (!stream) return std::nullopt;
    std::map<std::wstring, std::vector<uint32_t>> rvas;
    for (std::wstring line; std::getline(stream, line);) {
        size_t space = line.find(L' ');
        if (space == std::wstring::npos) return std::nullopt;
        std::vector<uint32_t>& found = rvas[line.substr(space + 1)];
        std::wstringstream list(line.substr(0, space));
        for (std::wstring rva; std::getline(list, rva, L',');) {
            if (rva == L"-") continue;
            wchar_t* end = nullptr;
            unsigned long value = wcstoul(rva.c_str(), &end, 16);
            if (rva.empty() || *end != L'\0') return std::nullopt;
            found.push_back(static_cast<uint32_t>(value));
        }
    }
    bool complete = std::all_of(names.begin(), names.end(), [&](const auto& name) { return rvas.contains(name); });
    return complete ? std::optional(rvas) : std::nullopt;
}

// Written to a temporary file first, so an interrupted write never leaves a half cache.
void WriteCache(const std::wstring& path, const std::map<std::wstring, std::vector<uint32_t>>& rvas) {
    std::wstring temporary = path + L".tmp";
    {
        std::wofstream stream(temporary, std::ios::trunc);
        for (const auto& [name, found] : rvas) {
            std::wstring list;
            for (uint32_t rva : found) {
                wchar_t hex[16];
                swprintf_s(hex, L"%x", rva);
                list += (list.empty() ? L"" : L",") + std::wstring(hex);
            }
            stream << (list.empty() ? L"-" : list) << L' ' << name << L'\n';
        }
        if (!stream) return;
    }
    MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

struct InternetHandle {
    HINTERNET handle;
    ~InternetHandle() {
        if (handle) WinHttpCloseHandle(handle);
    }
};

std::wstring Download(const std::wstring& path, const std::wstring& to, HANDLE cancel, DWORD& status) {
    InternetHandle session{WinHttpOpen(L"InstantAltTab", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0)};
    InternetHandle connection{
        session.handle ? WinHttpConnect(session.handle, kSymbolServer, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr};
    InternetHandle request{connection.handle ? WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr,
                                                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                                  WINHTTP_FLAG_SECURE)
                                             : nullptr};
    // Name resolution, connecting and sending may each take 10 s, and a stalled read 30 s.
    if (request.handle) WinHttpSetTimeouts(request.handle, 10000, 10000, 10000, 30000);
    auto cancelled = [&] { return cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0; };
    if (cancelled()) return L"stopped";
    if (!request.handle || !WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        return L"couldn't reach the symbol server (error " + std::to_wstring(GetLastError()) + L")";
    }
    if (cancelled()) return L"stopped";
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size,
                        nullptr);
    if (status != 200) return L"the symbol server answered " + std::to_wstring(status);

    std::ofstream file(to, std::ios::binary | std::ios::trunc);
    uint64_t total = 0;
    std::vector<char> buffer(1 << 16);
    for (DWORD read = 0;; total += read) {
        if (!WinHttpReadData(request.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            return L"the download broke off (error " + std::to_wstring(GetLastError()) + L")";
        }
        if (read == 0) break;
        if (cancelled()) return L"stopped";
        if (total + read > kMaxPdbBytes || !file.write(buffer.data(), read)) return L"couldn't save the download";
    }
    return {};
}

// dbghelp from System32, never a copy next to the agent.
struct Dbghelp {
    HMODULE module = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    decltype(&SymSetOptions) setOptions = Get<decltype(&SymSetOptions)>("SymSetOptions");
    decltype(&SymInitializeW) initialize = Get<decltype(&SymInitializeW)>("SymInitializeW");
    decltype(&SymLoadModuleExW) loadModule = Get<decltype(&SymLoadModuleExW)>("SymLoadModuleExW");
    decltype(&SymEnumSymbolsW) enumerate = Get<decltype(&SymEnumSymbolsW)>("SymEnumSymbolsW");
    decltype(&SymGetModuleInfoW64) moduleInfo = Get<decltype(&SymGetModuleInfoW64)>("SymGetModuleInfoW64");
    decltype(&SymCleanup) cleanup = Get<decltype(&SymCleanup)>("SymCleanup");

    template <typename F>
    F Get(const char* name) {
        return module ? reinterpret_cast<F>(GetProcAddress(module, name)) : nullptr;
    }
    bool Loaded() const { return setOptions && initialize && loadModule && enumerate && moduleInfo && cleanup; }
    ~Dbghelp() {
        if (module) FreeLibrary(module);
    }
};

std::wstring LookUp(const PeFile& file, const std::wstring& pdbDirectory, const std::vector<std::wstring>& names,
                    std::map<std::wstring, std::vector<uint32_t>>& rvas) {
    Dbghelp dbghelp;
    if (!dbghelp.Loaded()) return L"the system's dbghelp.dll couldn't be loaded";
    static int session;  // dbghelp identifies a session by a handle-sized value
    HANDLE id = &session;
    dbghelp.setOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!dbghelp.initialize(id, pdbDirectory.c_str(), FALSE)) return L"dbghelp couldn't start";
    std::wstring error;
    DWORD64 base =
        dbghelp.loadModule(id, nullptr, file.Path().c_str(), nullptr, file.ImageBase(), file.SizeOfImage(), nullptr, 0);
    IMAGEHLP_MODULEW64 info{sizeof(info)};
    if (!base || !dbghelp.moduleInfo(id, base, &info) || info.SymType != SymPdb) {
        error = L"dbghelp couldn't load the downloaded symbols";
    } else {
        // One pass over every public symbol: dbghelp's name masks don't match template arguments.
        for (const std::wstring& name : names) rvas[name];
        struct Context {
            std::map<std::wstring, std::vector<uint32_t>, std::less<>>* rvas;
            DWORD64 base;
        };
        std::map<std::wstring, std::vector<uint32_t>, std::less<>> wanted(rvas.begin(), rvas.end());
        Context context{&wanted, base};
        auto visit = [](PSYMBOL_INFOW symbol, ULONG, PVOID user) -> BOOL {
            auto& c = *static_cast<Context*>(user);
            auto entry = c.rvas->find(std::wstring_view(symbol->Name));  // NameLen may count the terminator
            if (entry != c.rvas->end()) {
                auto rva = static_cast<uint32_t>(symbol->Address - c.base);
                auto& found = entry->second;
                if (std::find(found.begin(), found.end(), rva) == found.end()) found.push_back(rva);
            }
            return TRUE;
        };
        dbghelp.enumerate(id, base, L"*", visit, &context);
        rvas.clear();
        rvas.insert(wanted.begin(), wanted.end());
    }
    dbghelp.cleanup(id);
    return error;
}

}  // namespace

Symbols LookUpSymbols(const PeFile& file, const std::vector<std::wstring>& names, const std::wstring& cacheDirectory,
                      HANDLE cancel) {
    Symbols symbols;
    auto debug = file.CodeView();
    if (!debug) {
        symbols.error = file.Path() + L" names no PDB";
        return symbols;
    }
    std::wstring key = SignatureKey(*debug);
    std::wstring cachePath = cacheDirectory + debug->pdbName + L"." + key + L".txt";
    if (auto cached = ReadCache(cachePath, names)) {
        symbols.rvas = std::move(*cached);
        return symbols;
    }

    std::wstring downloadDirectory = cacheDirectory + L"download\\";
    CreateDirectoryW(cacheDirectory.c_str(), nullptr);
    CreateDirectoryW(downloadDirectory.c_str(), nullptr);
    std::wstring pdbPath = downloadDirectory + debug->pdbName;
    std::wstring urlPath = L"/download/symbols/" + debug->pdbName + L"/" + key + L"/" + debug->pdbName;
    DWORD status = 0;
    if (std::wstring failure = Download(urlPath, pdbPath, cancel, status); !failure.empty()) {
        DeleteFileW(pdbPath.c_str());
        symbols.retryLater = true;
        symbols.notPublished = status == 404;
        symbols.error = symbols.notPublished
                            ? L"Microsoft hasn't published the public symbols of " + file.Path() + L" yet"
                            : L"downloading the symbols of " + file.Path() + L" failed: " + failure;
        return symbols;
    }
    symbols.error = LookUp(file, downloadDirectory, names, symbols.rvas);
    DeleteFileW(pdbPath.c_str());
    RemoveDirectoryW(downloadDirectory.c_str());
    if (symbols.error.empty()) WriteCache(cachePath, symbols.rvas);
    return symbols;
}

}  // namespace Discovery
