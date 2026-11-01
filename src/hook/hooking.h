#pragma once

// Thin layer over MinHook: hooks are created only on pinned modules whose
// function entry bytes match, and every hook body counts itself so the DLL can
// wait for in-flight calls before it is unloaded.

#include "pins.h"

#include <windows.h>

#include <atomic>
#include <optional>

// Put one at the top of every hook body.
class HookScope {
public:
    HookScope() {
        ++active_;
        ++depthOnThread_;
    }
    ~HookScope() {
        --active_;
        --depthOnThread_;
    }
    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;

    static int Active() { return active_; }
    // True while the calling thread is inside a hook (a COM call in a hooked
    // function can pump messages, so deferred work checks this first).
    static bool InsideHookOnThisThread() { return depthOnThread_ > 0; }

private:
    static inline std::atomic<int> active_{0};
    static inline thread_local int depthOnThread_ = 0;
};

struct ModuleImage {
    BYTE* base = nullptr;
    DWORD size = 0;

    bool Contains(const void* address) const {
        auto p = static_cast<const BYTE*>(address);
        return p >= base && p < base + size;
    }
    template <typename T>
    T At(DWORD rva) const {
        return reinterpret_cast<T>(base + rva);
    }
};

// The loaded module, if it is exactly the pinned build.
std::optional<ModuleImage> FindPinnedModule(const pins::Module& module);

bool EntryMatches(const ModuleImage& module, const pins::Function& function);

// Creates a (disabled) hook after checking the entry bytes; `original` receives
// the trampoline that calls the real function.
bool CreateHook(const ModuleImage& module, const pins::Function& function, void* detour, void** original);

template <typename Fn>
bool CreateHook(const ModuleImage& module, const pins::Function& function, Fn detour, Fn* original) {
    return CreateHook(module, function, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}

bool InitializeHooks();
bool EnableHooks();
void DisableHooks();
// After DisableHooks: waits for calls still inside hooks, then frees the
// trampolines. Returns false (and frees nothing) if one is still running.
bool ReleaseHooks(DWORD timeoutMs);

// Reads a pointer-sized value at `address + offset`, or null if it isn't readable.
void* ReadPointer(const void* address, size_t offset);
