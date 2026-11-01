#pragma once

// Starting, finding and stopping the background agent from another process.

#include <windows.h>

#include <string>

namespace AgentClient {

bool IsRunning();
// Starts the agent detached, with its own folder as working directory.
bool Launch(const std::wstring& agentExecutable);
// Asks a running agent to remove the hooks from Explorer and exit; returns
// false if it didn't exit within the timeout.
bool RequestStop(DWORD timeoutMs);

}  // namespace AgentClient
