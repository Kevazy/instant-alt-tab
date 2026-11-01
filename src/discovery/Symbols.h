#pragma once

// Public symbols from Microsoft's symbol server. The first time a build of a
// DLL is seen, its PDB (~30 MB for twinui.pcshell.dll) is downloaded, the
// requested names are looked up with the system's dbghelp, and only those
// results are kept, in a small text file named after the PDB's signature. The
// PDB itself is deleted.

#include "PeFile.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Discovery {

struct Symbols {
    std::map<std::wstring, std::vector<uint32_t>> rvas;  // every requested name; empty if the PDB doesn't have it
    std::wstring error;                                  // empty on success
    bool retryLater = false;                             // the download failed; it may work later
};

// `cancel`, if set, stops a download in progress.
Symbols LookUpSymbols(const PeFile& file, const std::vector<std::wstring>& names, const std::wstring& cacheDirectory,
                      HANDLE cancel = nullptr);

}  // namespace Discovery
