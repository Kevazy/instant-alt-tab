#include "hooking.h"

#include <MinHook.h>

#include <cstring>

std::optional<ModuleImage> FindPinnedModule(const pins::Module& module) {
    auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(module.name));
    if (!base) return std::nullopt;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != module.timeDateStamp || nt->OptionalHeader.SizeOfImage != module.sizeOfImage) {
        return std::nullopt;
    }
    return ModuleImage{base, nt->OptionalHeader.SizeOfImage};
}

bool EntryMatches(const ModuleImage& module, const pins::Function& function) {
    return std::memcmp(module.base + function.rva, function.entry.data(), function.entry.size()) == 0;
}

bool CreateHook(const ModuleImage& module, const pins::Function& function, void* detour, void** original) {
    return EntryMatches(module, function) && MH_CreateHook(module.base + function.rva, detour, original) == MH_OK;
}

bool InitializeHooks() { return MH_Initialize() == MH_OK; }

bool EnableHooks() { return MH_EnableHook(MH_ALL_HOOKS) == MH_OK; }

void DisableHooks() { MH_DisableHook(MH_ALL_HOOKS); }

bool ReleaseHooks(DWORD timeoutMs) {
    // A thread can be between a patched entry and its HookScope for a few
    // instructions, so give it a moment before counting.
    Sleep(100);
    for (DWORD waited = 0; HookScope::Active() > 0 && waited < timeoutMs; waited += 20) Sleep(20);
    if (HookScope::Active() > 0) return false;
    MH_Uninitialize();
    return true;
}

void* ReadPointer(const void* address, size_t offset) {
    __try {
        return *reinterpret_cast<void* const*>(static_cast<const BYTE*>(address) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
