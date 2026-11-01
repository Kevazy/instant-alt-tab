#include "pe_file.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace discovery {

namespace {

constexpr uint8_t kChainedUnwindInfo = 0x4;  // UNW_FLAG_CHAININFO
constexpr size_t kMaxChainDepth = 32;        // split functions have a few parts; a longer chain is corrupt

template <typename T>
bool ReadAt(const std::vector<uint8_t>& data, size_t offset, T& value) {
    if (offset > data.size() || data.size() - offset < sizeof(T)) return false;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return true;
}

}  // namespace

std::optional<PeFile> PeFile::Load(const std::wstring& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    PeFile file;
    file.path_ = path;
    file.data_.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());

    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!ReadAt(file.data_, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || !ReadAt(file.data_, dos.e_lfanew, nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return std::nullopt;
    }
    file.timeDateStamp_ = nt.FileHeader.TimeDateStamp;
    file.sizeOfImage_ = nt.OptionalHeader.SizeOfImage;
    file.imageBase_ = nt.OptionalHeader.ImageBase;
    file.debugDirectory_ = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];

    size_t sectionTable =
        dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER header;
        if (!ReadAt(file.data_, sectionTable + i * sizeof(header), header)) return std::nullopt;
        file.sections_.push_back({header.VirtualAddress, std::max(header.Misc.VirtualSize, header.SizeOfRawData),
                                  header.PointerToRawData, header.SizeOfRawData,
                                  (header.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
    }

    const IMAGE_DATA_DIRECTORY& exceptions = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto table = file.Read(exceptions.VirtualAddress, exceptions.Size);
    file.functions_.resize(table.size() / sizeof(RUNTIME_FUNCTION));
    std::memcpy(file.functions_.data(), table.data(), file.functions_.size() * sizeof(RUNTIME_FUNCTION));
    std::sort(file.functions_.begin(), file.functions_.end(),
              [](const auto& a, const auto& b) { return a.BeginAddress < b.BeginAddress; });
    return file;
}

const PeFile::Section* PeFile::SectionOf(uint32_t rva) const {
    for (const Section& section : sections_) {
        if (rva >= section.rva && rva - section.rva < section.size) return &section;
    }
    return nullptr;
}

std::span<const uint8_t> PeFile::Read(uint32_t rva, size_t count) const {
    const Section* section = SectionOf(rva);
    if (!section) return {};
    size_t inSection = rva - section->rva;
    if (inSection + count > section->fileSize) return {};  // past the bytes the file holds
    return {data_.data() + section->fileOffset + inSection, count};
}

bool PeFile::IsCode(uint32_t rva) const {
    const Section* section = SectionOf(rva);
    return section && section->code;
}

const RUNTIME_FUNCTION* PeFile::EntryContaining(uint32_t rva) const {
    auto next = std::upper_bound(functions_.begin(), functions_.end(), rva,
                                 [](uint32_t value, const auto& entry) { return value < entry.BeginAddress; });
    if (next == functions_.begin()) return nullptr;
    const RUNTIME_FUNCTION& entry = *std::prev(next);
    return rva < entry.EndAddress ? &entry : nullptr;
}

std::optional<uint32_t> PeFile::FunctionContaining(uint32_t rva) const {
    const RUNTIME_FUNCTION* entry = EntryContaining(rva);
    for (size_t depth = 0; entry && depth < kMaxChainDepth; ++depth) {
        auto header = Read(entry->UnwindInfoAddress & ~1u, 4);
        if (header.empty()) return std::nullopt;
        if (!((header[0] >> 3) & kChainedUnwindInfo)) return entry->BeginAddress;
        // The chained entry follows the unwind codes, which are padded to an even count.
        size_t codes = (header[2] + 1u) & ~1u;
        auto chained =
            Read((entry->UnwindInfoAddress & ~1u) + 4 + static_cast<uint32_t>(codes) * 2, sizeof(RUNTIME_FUNCTION));
        if (chained.empty()) return std::nullopt;
        RUNTIME_FUNCTION parent;
        std::memcpy(&parent, chained.data(), sizeof(parent));
        entry = EntryContaining(parent.BeginAddress);
    }
    return std::nullopt;
}

std::optional<uint32_t> PeFile::FunctionEnd(uint32_t start) const {
    const RUNTIME_FUNCTION* entry = EntryContaining(start);
    if (!entry || entry->BeginAddress != start) return std::nullopt;
    return entry->EndAddress;
}

std::vector<uint32_t> PeFile::Find(std::span<const uint8_t> bytes) const {
    std::vector<uint32_t> found;
    for (const Section& section : sections_) {
        if (section.fileOffset > data_.size()) continue;
        auto begin = data_.begin() + section.fileOffset;
        auto end = begin + std::min<size_t>(section.fileSize, data_.size() - section.fileOffset);
        for (auto at = std::search(begin, end, bytes.begin(), bytes.end()); at != end;
             at = std::search(at + 1, end, bytes.begin(), bytes.end())) {
            found.push_back(section.rva + static_cast<uint32_t>(at - begin));
        }
    }
    return found;
}

bool PeFile::IsReferencedFromData(uint32_t rva) const {
    uint64_t address = imageBase_ + rva;
    auto bytes = std::span(reinterpret_cast<const uint8_t*>(&address), sizeof(address));
    for (uint32_t at : Find(bytes)) {
        if (!IsCode(at) && at % sizeof(address) == 0) return true;
    }
    return false;
}

std::set<uint32_t> PeFile::CodePointersInData() const {
    std::set<uint32_t> found;
    for (const Section& section : sections_) {
        if (section.code || section.fileOffset > data_.size()) continue;
        size_t size = std::min<size_t>(section.fileSize, data_.size() - section.fileOffset);
        for (size_t at = 0; at + sizeof(uint64_t) <= size; at += sizeof(uint64_t)) {
            uint64_t value;
            std::memcpy(&value, data_.data() + section.fileOffset + at, sizeof(value));
            if (value > imageBase_ && value - imageBase_ < sizeOfImage_ &&
                IsCode(static_cast<uint32_t>(value - imageBase_))) {
                found.insert(static_cast<uint32_t>(value - imageBase_));
            }
        }
    }
    return found;
}

std::optional<PeFile::DebugInfo> PeFile::CodeView() const {
    constexpr uint32_t kRsds = 0x53445352;  // "RSDS": a PDB 7.0 reference
    auto directory = Read(debugDirectory_.VirtualAddress, debugDirectory_.Size);
    for (size_t at = 0; at + sizeof(IMAGE_DEBUG_DIRECTORY) <= directory.size(); at += sizeof(IMAGE_DEBUG_DIRECTORY)) {
        IMAGE_DEBUG_DIRECTORY entry;
        std::memcpy(&entry, directory.data() + at, sizeof(entry));
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        auto record = Read(entry.AddressOfRawData, entry.SizeOfData);
        uint32_t signature;
        if (record.size() < 24 || (std::memcpy(&signature, record.data(), 4), signature != kRsds)) continue;
        DebugInfo info;
        std::memcpy(&info.guid, record.data() + 4, sizeof(GUID));
        std::memcpy(&info.age, record.data() + 20, sizeof(uint32_t));
        auto name = reinterpret_cast<const char*>(record.data() + 24);
        size_t length = strnlen(name, record.size() - 24);
        std::string narrow(name, length);
        narrow = narrow.substr(narrow.find_last_of("\\/") + 1);  // some records hold a full build path
        info.pdbName.assign(narrow.begin(), narrow.end());
        return info;
    }
    return std::nullopt;
}

}  // namespace discovery
