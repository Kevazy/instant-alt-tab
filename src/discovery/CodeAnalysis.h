#pragma once

// Reads machine code by meaning rather than by its bytes: instructions are
// decoded (Zydis), and a function is walked in address order while tracking
// what each register holds: a parameter, an address or a field relative to a
// parameter, a constant, or a global. Rules can then say "the first argument
// of this call is parameter 0 plus 0xa8" however the compiler encoded it.
//
// The walk is linear and ignores branches, so a value is what the most recent
// instruction before it in the code set. That is all the rules need, and each
// rule also requires its match to be unique.

#include "PeFile.h"

#include <Zydis.h>

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

namespace Discovery {

enum Register : uint8_t {
    kRax,
    kRcx,
    kRdx,
    kRbx,
    kRsp,
    kRbp,
    kRsi,
    kRdi,
    kR8,
    kR9,
    kR10,
    kR11,
    kR12,
    kR13,
    kR14,
    kR15
};
inline constexpr int kRegisters = 16;
inline constexpr int kNoRegister = -1;
inline constexpr int kRip = kRegisters;

// A register's content. A pointer is described by how it was reached from a
// parameter: start at the parameter, then for each load add its offset and
// read the qword there, then add `number`. So [[rcx]+0x98] is parameter 0,
// offsets {0, 0x98}, number 0; and rcx+0xa8 is parameter 0, no loads, 0xa8.
struct Value {
    enum class Kind : uint8_t { Unknown, Constant, Pointer, GlobalAddress };
    static constexpr int kMaxLoads = 4;

    Kind kind = Kind::Unknown;
    uint8_t parameter = 0;  // rcx, rdx, r8, r9 on entry
    uint8_t loads = 0;
    std::array<int32_t, kMaxLoads> offsets{};
    int64_t number = 0;  // the constant, the offset after the last load, or a global's RVA

    auto operator<=>(const Value&) const = default;

    static Value Constant(int64_t n) { return {Kind::Constant, 0, 0, {}, n}; }
    static Value Parameter(uint8_t p) { return {Kind::Pointer, p, 0, {}, 0}; }
    // parameter + offset
    static Value AddressIn(uint8_t p, int64_t offset) { return {Kind::Pointer, p, 0, {}, offset}; }
    // the qword at parameter + offset
    static Value FieldOf(uint8_t p, int32_t offset) { return {Kind::Pointer, p, 1, {offset}, 0}; }
};

struct Memory {
    int base = kNoRegister;  // a Register, kRip or kNoRegister
    int index = kNoRegister;
    int64_t displacement = 0;
    std::optional<uint32_t> rva;  // the address, for a RIP-relative operand
    uint16_t bits = 0;
    bool written = false;
    Value baseValue;  // what the base register held
};

struct Step {
    uint32_t rva = 0;
    uint8_t length = 0;
    ZydisMnemonic mnemonic = ZYDIS_MNEMONIC_INVALID;
    bool call = false, jump = false, conditionalJump = false, ret = false;
    std::optional<uint32_t> target;  // of a direct call or jump
    std::optional<Memory> memory;    // the explicit memory operand, if any
    std::optional<int64_t> immediate;
    std::array<Value, kRegisters> before;  // what each register held before this instruction

    const Value& Argument(int n) const;  // rcx, rdx, r8, r9 before a call
};

// Decodes from `start` to `end` (or, without an end, up to the first return or
// unconditional jump), at most `maxSteps` instructions. Registers start as the
// four parameters when `startsWithParameters`, unknown otherwise (a split-off
// part of a function).
std::vector<Step> Walk(const PeFile& file, uint32_t start, std::optional<uint32_t> end = std::nullopt,
                       bool startsWithParameters = true, size_t maxSteps = SIZE_MAX);

// A whole function from the function table.
std::vector<Step> WalkFunction(const PeFile& file, uint32_t function);

struct CallSite {
    uint32_t function;  // containing function (its main part)
    uint32_t rva;
    uint32_t target;
    Value rcx, rdx;
};

struct Reference {
    uint32_t function;
    uint32_t rva;  // the global a RIP-relative operand addresses
};

// Every direct call and every RIP-relative reference in the module, decoded once.
class CodeIndex {
public:
    explicit CodeIndex(const PeFile& file);
    std::vector<CallSite> CallsTo(uint32_t target) const;
    std::vector<CallSite> CallsFrom(uint32_t function) const;
    std::vector<uint32_t> FunctionsReferencing(uint32_t rva) const;
    std::set<uint32_t> CallTargets() const;

private:
    std::vector<CallSite> m_calls;
    std::vector<Reference> m_references;
};

// The vtable slot a wrapper calls through: the method loaded from
// [[parameter 0] ...] (`depth` loads of offset 0 reach the vtable: 1 when the
// parameter is the interface pointer, 2 when it points to one) and called.
std::optional<uint32_t> VtableSlotCalled(const std::vector<Step>& steps, int depth);

}  // namespace Discovery
