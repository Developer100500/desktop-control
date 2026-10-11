#pragma once
#include <windows.h>
#include <cstdint>

// Native MCP hosts: call immediately before forwarding activate_window on a
// foreground/user-initiated path. Never use ASFW_ANY or cache success.
namespace dc::foreground
{
inline uint64_t creation_time(HANDLE process)
{
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exit, &kernel, &user))
        return 0;
    return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
inline DWORD grant(DWORD server_pid, uint64_t expected_creation)
{
    if (!server_pid || server_pid == ASFW_ANY || !expected_creation)
        return ERROR_INVALID_PARAMETER;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, server_pid);
    if (!process)
        return GetLastError();
    DWORD own_session{}, server_session{}, error = ERROR_SUCCESS;
    if (creation_time(process) != expected_creation || WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
        error = ERROR_INVALID_PARAMETER;
    else if (!ProcessIdToSessionId(GetCurrentProcessId(), &own_session) ||
             !ProcessIdToSessionId(server_pid, &server_session))
        error = GetLastError();
    else if (own_session != server_session)
        error = ERROR_ACCESS_DENIED;
    else
    {
        SetLastError(ERROR_SUCCESS);
        if (!AllowSetForegroundWindow(server_pid))
            error = GetLastError() ? GetLastError() : ERROR_ACCESS_DENIED;
    }
    CloseHandle(process);
    return error;
}
} // namespace dc::foreground
