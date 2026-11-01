#include "hooking.h"

#include <MinHook.h>

#include <cstring>

std::optional<ModuleImage> FindModule(const wchar_t* name, const internals::Module& analysed) {
    auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(name));
    if (!base) return std::nullopt;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != analysed.timeDateStamp ||
        nt->OptionalHeader.SizeOfImage != analysed.sizeOfImage) {
        return std::nullopt;
    }
    return ModuleImage{base, nt->OptionalHeader.SizeOfImage};
}

bool IsCode(const ModuleImage& module, const void* address) {
    if (!module.Contains(address)) return false;
    auto nt =
        reinterpret_cast<IMAGE_NT_HEADERS64*>(module.base + reinterpret_cast<IMAGE_DOS_HEADER*>(module.base)->e_lfanew);
    auto rva = static_cast<DWORD>(static_cast<const BYTE*>(address) - module.base);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (rva >= section->VirtualAddress && rva - section->VirtualAddress < section->Misc.VirtualSize) {
            return (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        }
    }
    return false;
}

bool EntryMatches(const ModuleImage& module, const internals::Function& function) {
    const BYTE* entry = module.base + function.rva;
    return function.rva < module.size - sizeof(function.entry) && IsCode(module, entry) &&
           std::memcmp(entry, function.entry, sizeof(function.entry)) == 0;
}

bool CreateHook(const ModuleImage& module, const internals::Function& function, void* detour, void** original) {
    return EntryMatches(module, function) && MH_CreateHook(module.base + function.rva, detour, original) == MH_OK;
}

bool HookNow(void* target, void* detour, void** original) {
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    if (MH_EnableHook(target) == MH_OK) return true;
    MH_RemoveHook(target);
    return false;
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
