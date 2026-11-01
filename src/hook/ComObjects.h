#pragma once

// Reading COM objects whose layout discovery found: pointer members at an
// offset, vtable slots, and a check that a pointer plausibly is a live COM
// object before anything calls into it.

#include <unknwn.h>

#include <cstddef>

// The pointer member at `offset`, as a reference that can be compared or written.
IUnknown*& MemberAt(void* object, size_t offset);
// Entry `slot` of the object's vtable.
void* VtableSlot(IUnknown* object, size_t slot);
// Whether two interface pointers belong to the same object (COM identity).
bool SameObject(IUnknown* a, IUnknown* b);
// A pointer that looks like a COM object: its vtable and the vtable's first
// entry (QueryInterface) are in loaded modules. Checked before calling into a
// member read at a discovered offset.
bool LooksLikeComObject(const void* object);
// The COM pointer member at `offset`, or null if what's there isn't one.
IUnknown* ComMember(void* object, size_t offset);
