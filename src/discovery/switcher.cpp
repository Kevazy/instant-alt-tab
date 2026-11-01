// The switcher DLL has no public symbols. Its functions are found by what the
// code does: which functions call which, with which constant arguments, and
// which members of their parameters they read. Instruction encodings,
// registers and function prologues don't matter, so a recompile that
// changes only those still matches.

#include "rules.h"

#include <algorithm>
#include <map>

namespace discovery {

namespace {

// Windows.Foundation.Collections.CollectionChange values, which the vector's notifier takes.
constexpr int64_t kItemInserted = 1, kItemRemoved = 2;
constexpr size_t kPrologueSteps = 16;  // how far into a function its first test is looked for

bool IsParameter(const Value& value, uint8_t parameter = 0) { return value == Value::Parameter(parameter); }

bool IsFieldOf0(const Value& value, uint32_t& offset) {
    if (value.kind != Value::Kind::Pointer || value.parameter != 0 || value.loads != 1 || value.number != 0) {
        return false;
    }
    offset = static_cast<uint32_t>(value.offsets[0]);
    return true;
}

bool Reads(const std::vector<Step>& steps, uint32_t member) {
    return std::any_of(steps.begin(), steps.end(), [&](const Step& step) {
        return step.memory && !step.memory->written && IsParameter(step.memory->baseValue) &&
               step.memory->displacement == member;
    });
}

// The vector's change notifier tests a byte member of the vector before doing
// anything ("is anyone observing me?"): cmp byte [parameter 0 + X], 0; jcc.
std::optional<uint32_t> ObservedFlagTestedBy(const std::vector<Step>& steps) {
    for (size_t i = 0; i < steps.size() && i < kPrologueSteps; ++i) {
        const Step& step = steps[i];
        bool test = step.mnemonic == ZYDIS_MNEMONIC_CMP && step.memory && step.memory->bits == 8 &&
                    step.memory->index == kNoRegister && IsParameter(step.memory->baseValue) && step.immediate == 0;
        if (test && i + 1 < steps.size() && steps[i + 1].conditionalJump) {
            return static_cast<uint32_t>(step.memory->displacement);
        }
    }
    return std::nullopt;
}

struct CardVector {
    uint32_t notify, removeAt, insertAt, observed;
    uint32_t sync, cards;  // the list function that edits the vector, and the list member holding it
};

// The vector type is found from its notifier: RemoveAt calls it with
// ItemRemoved and InsertAt with ItemInserted. The list's re-sync is the one
// function calling both on the same member of its parameter, the card vector.
std::vector<CardVector> FindCardVectors(const PeFile& file, const CodeIndex& code) {
    std::vector<CardVector> found;
    for (const RUNTIME_FUNCTION& entry : file.Functions()) {
        if (file.FunctionContaining(entry.BeginAddress) != entry.BeginAddress) continue;
        auto observed =
            ObservedFlagTestedBy(Walk(file, entry.BeginAddress, entry.EndAddress, true, kPrologueSteps + 1));
        if (!observed) continue;
        std::set<uint32_t> removers, inserters;
        for (const CallSite& call : code.CallsTo(entry.BeginAddress)) {
            if (call.rdx == Value::Constant(kItemRemoved)) removers.insert(call.function);
            if (call.rdx == Value::Constant(kItemInserted)) inserters.insert(call.function);
        }
        // sync -> member -> {RemoveAt, InsertAt} it calls on that member
        std::map<uint32_t, std::map<uint32_t, std::pair<std::set<uint32_t>, std::set<uint32_t>>>> users;
        for (bool remove : {true, false}) {
            for (uint32_t function : remove ? removers : inserters) {
                for (const CallSite& call : code.CallsTo(function)) {
                    uint32_t member;
                    if (!IsFieldOf0(call.rcx, member)) continue;
                    auto& [r, i] = users[call.function][member];
                    (remove ? r : i).insert(function);
                }
            }
        }
        for (const auto& [sync, members] : users) {
            if (members.size() != 1) continue;
            const auto& [member, functions] = *members.begin();
            if (functions.first.size() == 1 && functions.second.size() == 1) {
                found.push_back(
                    {entry.BeginAddress, *functions.first.begin(), *functions.second.begin(), *observed, sync, member});
            }
        }
    }
    return found;
}

// The sort is the list's virtual method that the re-sync calls on the list
// itself and that reads the card vector member.
uint32_t FindSort(const PeFile& file, const CodeIndex& code, const CardVector& vector) {
    std::set<uint32_t> sorts;
    for (const CallSite& call : code.CallsFrom(vector.sync)) {
        if (IsParameter(call.rcx) && file.IsReferencedFromData(call.target) &&
            Reads(WalkFunction(file, call.target), vector.cards)) {
            sorts.insert(call.target);
        }
    }
    return Unique(L"card list sort", sorts);
}

// RemoveAt reads the vector's storage member, a begin/end pair: [[vector + X]] and [[vector + X] + 8].
uint32_t FindStorage(const PeFile& file, uint32_t removeAt) {
    std::set<uint32_t> storage;
    for (const Step& step : WalkFunction(file, removeAt)) {
        uint32_t member;
        if (step.memory && !step.memory->written && IsFieldOf0(step.memory->baseValue, member) &&
            step.memory->displacement == sizeof(void*)) {
            storage.insert(member);
        }
    }
    return Unique(L"card vector storage", storage);
}

// When a card is chosen, the list marks itself closed and unsubscribes from
// the window list: mov byte [list + X], 1, then a jump to a function that
// first checks its subscription token (cmp qword [list + Y], 0). Such a tiny
// function may have no function table entry, so the candidates are every
// function start, call target and code pointer in data, and every code
// address after int3 padding.
std::pair<uint32_t, uint32_t> FindCloseList(const PeFile& file, const CodeIndex& code) {
    std::set<uint32_t> candidates = code.CallTargets();
    candidates.merge(file.CodePointersInData());
    for (const RUNTIME_FUNCTION& entry : file.Functions()) candidates.insert(entry.BeginAddress);
    constexpr uint8_t kInt3 = 0xCC;
    for (uint32_t padding : file.Find(std::span(&kInt3, 1))) {
        auto next = file.Read(padding + 1, 1);
        if (!next.empty() && next[0] != kInt3) candidates.insert(padding + 1);
    }
    std::set<std::pair<uint32_t, uint32_t>> found;
    for (uint32_t start : candidates) {
        if (!file.IsCode(start)) continue;
        std::vector<Step> steps = Walk(file, start, std::nullopt, true, 2);
        if (steps.size() != 2 || !steps[1].jump || !steps[1].target) continue;
        const Step& mark = steps[0];
        if (mark.mnemonic != ZYDIS_MNEMONIC_MOV || !mark.memory || !mark.memory->written || mark.memory->bits != 8 ||
            !IsParameter(mark.memory->baseValue) || mark.immediate != 1) {
            continue;
        }
        std::vector<Step> target = Walk(file, *steps[1].target, file.FunctionEnd(*steps[1].target), true, 6);
        bool checksToken = std::any_of(target.begin(), target.end(), [](const Step& step) {
            return step.mnemonic == ZYDIS_MNEMONIC_CMP && step.memory && step.memory->bits == 64 &&
                   IsParameter(step.memory->baseValue) && step.immediate == 0;
        });
        if (checksToken) found.insert({start, static_cast<uint32_t>(mark.memory->displacement)});
    }
    return Unique(L"card list close", found);
}

// The descendant search is the recursive function that references the searched type's interface ID.
uint32_t FindDescendantSearch(const PeFile& file, const CodeIndex& code) {
    auto id = std::span(reinterpret_cast<const uint8_t*>(&internals::kSearchedType), sizeof(GUID));
    std::set<uint32_t> found;
    for (uint32_t at : file.Find(id)) {
        for (uint32_t function : code.FunctionsReferencing(at)) {
            auto calls = code.CallsFrom(function);
            if (std::any_of(calls.begin(), calls.end(), [&](const CallSite& c) { return c.target == function; })) {
                found.insert(function);
            }
        }
    }
    return Unique(L"descendant search", found);
}

}  // namespace

void FindSwitcher(const PeFile& file, internals::Switcher& switcher) {
    CodeIndex code(file);
    std::vector<CardVector> vectors = FindCardVectors(file, code);
    if (vectors.size() != 1) {
        throw RuleFailed{L"card vector: expected one match, found " + std::to_wstring(vectors.size())};
    }
    const CardVector& vector = vectors.front();
    switcher.vectorNotify = FunctionAt(file, vector.notify, L"vector notifier");
    switcher.vectorRemoveAt = FunctionAt(file, vector.removeAt, L"vector RemoveAt");
    switcher.vectorInsertAt = FunctionAt(file, vector.insertAt, L"vector InsertAt");
    switcher.vectorObserved = vector.observed;
    switcher.vectorStorage = FindStorage(file, vector.removeAt);
    switcher.syncList = FunctionAt(file, vector.sync, L"card list re-sync");
    switcher.listCards = vector.cards;
    switcher.sortList = FunctionAt(file, FindSort(file, code, vector), L"card list sort");
    auto [close, closed] = FindCloseList(file, code);
    switcher.closeList = FunctionAt(file, close, L"card list close");
    switcher.listClosed = closed;
    switcher.findDescendant = FunctionAt(file, FindDescendantSearch(file, code), L"descendant search");
}

}  // namespace discovery
