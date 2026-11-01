#pragma once

// A DLL file read from disk and addressed by RVA, with the parts of the PE
// format discovery needs: sections, the function table and the debug
// directory.

#include <windows.h>

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace discovery {

class PeFile {
public:
    static std::optional<PeFile> Load(const std::wstring& path);

    const std::wstring& Path() const { return path_; }
    uint32_t TimeDateStamp() const { return timeDateStamp_; }
    uint32_t SizeOfImage() const { return sizeOfImage_; }
    uint64_t ImageBase() const { return imageBase_; }

    // `count` bytes at `rva`, or an empty span if they aren't all in one section.
    std::span<const uint8_t> Read(uint32_t rva, size_t count) const;
    bool IsCode(uint32_t rva) const;

    // Functions from the exception directory. Every non-leaf function has an
    // entry; a function split into several parts has one per part.
    const std::vector<RUNTIME_FUNCTION>& Functions() const { return functions_; }
    // The start of the function containing `rva`, following split parts back to the main one.
    std::optional<uint32_t> FunctionContaining(uint32_t rva) const;
    // The end of the part that starts at `start`.
    std::optional<uint32_t> FunctionEnd(uint32_t start) const;

    // Every RVA where `bytes` occurs.
    std::vector<uint32_t> Find(std::span<const uint8_t> bytes) const;
    // Whether a non-code section holds an absolute pointer to `rva` (a vtable entry, typically).
    bool IsReferencedFromData(uint32_t rva) const;
    // Every code address stored as an absolute pointer in a non-code section (vtables, function tables).
    std::set<uint32_t> CodePointersInData() const;

    struct DebugInfo {
        std::wstring pdbName;
        GUID guid;
        uint32_t age;
    };
    std::optional<DebugInfo> CodeView() const;

private:
    struct Section {
        uint32_t rva, size, fileOffset, fileSize;
        bool code;
    };
    const Section* SectionOf(uint32_t rva) const;
    const RUNTIME_FUNCTION* EntryContaining(uint32_t rva) const;

    std::wstring path_;
    std::vector<uint8_t> data_;
    std::vector<Section> sections_;
    std::vector<RUNTIME_FUNCTION> functions_;  // sorted by BeginAddress
    uint32_t timeDateStamp_ = 0, sizeOfImage_ = 0;
    uint64_t imageBase_ = 0;
    IMAGE_DATA_DIRECTORY debugDirectory_{};
};

}  // namespace discovery
