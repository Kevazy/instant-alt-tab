#include "card_list.h"

#include "../shared/log.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace card_list {
namespace {

using ListFn = void*(__fastcall*)(void* list, void*, void*, void*);
using RemoveAtFn = void(__fastcall*)(void* vector, unsigned index);
using InsertAtFn = void(__fastcall*)(void* vector, unsigned index, IInspectable* card);
using NotifyFn = void(__fastcall*)(void* vector, unsigned change, unsigned index);

constexpr unsigned kCollectionReset = 0;     // Windows.Foundation.Collections.CollectionChange.Reset
constexpr size_t kMaxReplayCards = 2048;     // beyond this a single Reset is cheaper than diffing
constexpr size_t kMaxPlausibleCards = 8192;  // far more windows than a session has: a bigger "vector" isn't one

ModuleImage g_switcher;
internals::Switcher g_found;  // what discovery found in it
ListFn g_syncList, g_sortList;
RemoveAtFn g_removeAt;
InsertAtFn g_insertAt;
NotifyFn g_notify;
std::atomic<void*> g_quietVector{nullptr};
thread_local void* t_lastSortedList = nullptr;
thread_local unsigned t_sortCount = 0;

BYTE& Observed(void* vector) { return *(static_cast<BYTE*>(vector) + g_found.vectorObserved); }

// The cards in a vector at one moment, each with a reference held.
class Snapshot {
public:
    explicit Snapshot(void* vector) {
        for (IInspectable* card : Cards(vector)) {
            if (card) card->AddRef();
            cards_.push_back(card);
        }
    }
    ~Snapshot() {
        for (IInspectable* card : cards_) {
            if (card) card->Release();
        }
    }
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;

    const std::vector<IInspectable*>& cards() const { return cards_; }
    bool operator==(const Snapshot& other) const { return cards_ == other.cards_; }

private:
    std::vector<IInspectable*> cards_;
};

// Marks, in `keepA` and `keepB`, the cards of one longest common subsequence of a and b.
void LongestCommonSubsequence(const std::vector<IInspectable*>& a, const std::vector<IInspectable*>& b,
                              std::vector<bool>& keepA, std::vector<bool>& keepB) {
    const size_t n = a.size(), m = b.size();
    std::vector<uint16_t> length((n + 1) * (m + 1), 0);
    auto at = [&](size_t i, size_t j) -> uint16_t& { return length[i * (m + 1) + j]; };
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            at(i, j) = a[i] == b[j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
        }
    }
    keepA.assign(n, false);
    keepB.assign(m, false);
    for (size_t i = 0, j = 0; i < n && j < m;) {
        if (a[i] == b[j]) {
            keepA[i++] = keepB[j++] = true;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            ++i;
        } else {
            ++j;
        }
    }
}

// Turns the vector from `after` back into `before` silently, then replays the
// minimal edits from `before` to `after` with notifications on, so XAML sees a
// consistent list at every step. Returns whether the result is `after`.
bool ReplayMinimalEdits(void* vector, const Snapshot& before, const Snapshot& after) {
    const auto& from = before.cards();
    const auto& to = after.cards();
    if (from.size() > kMaxReplayCards || to.size() > kMaxReplayCards) return false;

    for (size_t i = to.size(); i-- > 0;) g_removeAt(vector, static_cast<unsigned>(i));
    for (size_t i = 0; i < from.size(); ++i) g_insertAt(vector, static_cast<unsigned>(i), from[i]);
    Observed(vector) = 1;

    std::vector<bool> keepFrom, keepTo;
    LongestCommonSubsequence(from, to, keepFrom, keepTo);
    for (size_t i = from.size(); i-- > 0;) {
        if (!keepFrom[i]) g_removeAt(vector, static_cast<unsigned>(i));
    }
    for (size_t j = 0; j < to.size(); ++j) {
        if (!keepTo[j]) g_insertAt(vector, static_cast<unsigned>(j), to[j]);
    }
    return Snapshot(vector) == after;
}

// Runs `edit` (a switcher function that rewrites the list's cards) with change
// notifications off, then shows XAML the result as minimal edits. Only the
// kept menu's list, and only while XAML observes it and it isn't already being
// edited quietly (a sort runs inside a re-sync).
template <typename Edit>
void* EditQuietly(void* list, Edit&& edit) {
    void* vector = CardVectorOf(list);
    if (!vector || vector != g_quietVector || Observed(vector) != 1) return edit();

    Snapshot before(vector);
    Observed(vector) = 0;
    void* result;
    try {
        result = edit();
    } catch (...) {
        Observed(vector) = 1;
        g_notify(vector, kCollectionReset, 0);
        throw;
    }
    Snapshot after(vector);
    bool consistent = true;
    if (!(after == before)) {
        try {
            consistent = ReplayMinimalEdits(vector, before, after);
        } catch (...) {
            consistent = false;
        }
    }
    Observed(vector) = 1;
    if (!consistent) {
        g_notify(vector, kCollectionReset, 0);  // XAML rebuilds from the vector as it is
        Log(L"card list replay did not reproduce the list; sent a reset");
    }
    return result;
}

void* __fastcall SyncListHook(void* list, void* a, void* b, void* c) {
    HookScope scope;
    return EditQuietly(list, [&] { return g_syncList(list, a, b, c); });
}

void* __fastcall SortListHook(void* list, void* a, void* b, void* c) {
    HookScope scope;
    t_lastSortedList = list;
    ++t_sortCount;
    return EditQuietly(list, [&] { return g_sortList(list, a, b, c); });
}

}  // namespace

bool Install(const ModuleImage& switcher, const internals::Switcher& found) {
    g_switcher = switcher;
    g_found = found;
    if (!EntryMatches(switcher, found.vectorRemoveAt) || !EntryMatches(switcher, found.vectorInsertAt) ||
        !EntryMatches(switcher, found.vectorNotify)) {
        return false;
    }
    g_removeAt = switcher.At<RemoveAtFn>(found.vectorRemoveAt.rva);
    g_insertAt = switcher.At<InsertAtFn>(found.vectorInsertAt.rva);
    g_notify = switcher.At<NotifyFn>(found.vectorNotify.rva);
    return CreateHook(switcher, found.syncList, &SyncListHook, &g_syncList) &&
           CreateHook(switcher, found.sortList, &SortListHook, &g_sortList);
}

void* CardVectorOf(void* list) {
    void* vector = list ? ReadPointer(list, g_found.listCards) : nullptr;
    if (!vector || !g_switcher.Contains(ReadPointer(vector, 0))) return nullptr;  // vtable in the switcher
    void* storage = ReadPointer(vector, g_found.vectorStorage);
    auto begin = static_cast<BYTE*>(storage ? ReadPointer(storage, 0) : nullptr);
    auto end = static_cast<BYTE*>(storage ? ReadPointer(storage, sizeof(void*)) : nullptr);
    bool sane = begin && end >= begin && static_cast<size_t>(end - begin) <= kMaxPlausibleCards * sizeof(void*);
    return sane ? vector : nullptr;
}

std::span<IInspectable* const> Cards(void* cardVector) {
    auto storage = static_cast<IInspectable** const*>(ReadPointer(cardVector, g_found.vectorStorage));
    if (!storage || !storage[0]) return {};
    return {storage[0], storage[1]};
}

void* LastSortedList() { return t_lastSortedList; }

unsigned SortCount() { return t_sortCount; }

void SetQuietVector(void* cardVector) { g_quietVector = cardVector; }

}  // namespace card_list
