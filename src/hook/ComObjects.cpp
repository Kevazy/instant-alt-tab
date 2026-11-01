#include "ComObjects.h"

#include "Hooking.h"

#include <winrt/base.h>

using winrt::com_ptr;

IUnknown*& MemberAt(void* object, size_t offset) {
    return *reinterpret_cast<IUnknown**>(static_cast<BYTE*>(object) + offset);
}

void* VtableSlot(IUnknown* object, size_t slot) { return (*reinterpret_cast<void***>(object))[slot]; }

bool SameObject(IUnknown* a, IUnknown* b) {
    if (!a || !b) return a == b;
    com_ptr<IUnknown> x, y;
    return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(x.put()))) && SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(y.put()))) &&
           x == y;
}

bool LooksLikeComObject(const void* object) {
    auto inModule = [](const void* address) {
        HMODULE module;
        return address &&
               GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCWSTR>(address), &module);
    };
    void* vtable = object ? ReadPointer(object, 0) : nullptr;
    return inModule(vtable) && inModule(ReadPointer(vtable, 0));
}

IUnknown* ComMember(void* object, size_t offset) {
    auto value = static_cast<IUnknown*>(ReadPointer(object, offset));
    return LooksLikeComObject(value) ? value : nullptr;
}
