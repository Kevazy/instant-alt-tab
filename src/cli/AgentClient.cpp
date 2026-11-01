#include "AgentClient.h"

#include "../shared/SharedNames.h"

namespace AgentClient {

bool IsRunning() {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, SharedNames::kAgentMutex);
    if (mutex) CloseHandle(mutex);
    return mutex != nullptr;
}

bool Launch(const std::wstring& agentExecutable) {
    std::wstring directory = agentExecutable.substr(0, agentExecutable.rfind(L'\\'));
    std::wstring commandLine = L"\"" + agentExecutable + L"\"";
    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(agentExecutable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, directory.c_str(), &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool RequestStop(DWORD timeoutMs) {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, SharedNames::kAgentMutex);
    if (!mutex) return true;  // not running
    // The agent owns the mutex until it exits. The event is set again while
    // waiting, in case the agent was still starting up and hadn't created it yet.
    bool stopped = false;
    for (DWORD waited = 0; !stopped && waited <= timeoutMs; waited += 250) {
        if (HANDLE stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, SharedNames::kAgentStopEvent)) {
            SetEvent(stop);
            CloseHandle(stop);
        }
        DWORD result = WaitForSingleObject(mutex, 250);
        stopped = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    if (stopped) ReleaseMutex(mutex);
    CloseHandle(mutex);
    return stopped;
}

}  // namespace AgentClient
