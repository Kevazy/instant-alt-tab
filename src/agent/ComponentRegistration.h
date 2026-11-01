#pragma once

// Where Windows registered a WinRT component DLL, read the way Windows reads
// it when a class from it is first used: from the DllPath of the activatable
// classes registered to that file. Lets the agent analyse and load a component
// before Explorer does.

#include <string>

// The full path of the component DLL with this file name, or an empty string
// if no class is registered to it or its classes name different files.
std::wstring RegisteredComponentPath(const wchar_t* fileName);
