#include "control.hpp"
#include "foreground_permission.hpp"
#include <algorithm>

namespace dc
{
json foreground_information()
{
    // Return process identity and legacy host-grant diagnostics; takes no parameters.
    DWORD foreground_pid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
    auto helper = std::filesystem::path(executable()).parent_path() / L"desktop-control-foreground.exe";
    return {{"server_pid", GetCurrentProcessId()},
            {"server_process_created", std::to_string(process_created(GetCurrentProcessId()))},
            {"foreground_pid", foreground_pid}, {"method", "AllowSetForegroundWindow"},
            {"helper_available", std::filesystem::is_regular_file(helper)}, {"prompt_timeout_ms", 15000}};
}
void request_control_permission(State &state, HANDLE job, Window const *window)
{
    // Ask for session access (window=nullptr), or perform the legacy window handoff.
    const bool session = window == nullptr;
    if (!session)
        state.require_active();
    auto generation = state.cancellation.load();
    auto helper = std::filesystem::path(executable()).parent_path() / L"desktop-control-foreground.exe";
    if (!std::filesystem::is_regular_file(helper))
        throw Error("FOREGROUND_HELPER_MISSING", "Keep desktop-control-foreground.exe next to the server", "focus");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE child_input{}, parent_input{}, parent_output{}, child_output{};
    check(CreatePipe(&child_input, &parent_input, &sa, 16384), "FOREGROUND_PIPE", "Cannot create request pipe");
    Handle input(child_input), write(parent_input);
    check(CreatePipe(&parent_output, &child_output, &sa, 16384), "FOREGROUND_PIPE", "Cannot create reply pipe");
    Handle read(parent_output), output(child_output);
    check(SetHandleInformation(write.value, HANDLE_FLAG_INHERIT, 0) &&
              SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0),
          "FOREGROUND_PIPE", "Cannot restrict pipe inheritance");
    SIZE_T size{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> storage(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    check(InitializeProcThreadAttributeList(attributes, 1, 0, &size), "FOREGROUND_START", "Cannot create handle list");
    struct Cleanup
    {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~Cleanup() { DeleteProcThreadAttributeList(value); }
    } cleanup{attributes};
    HANDLE inherited[]{input.value, output.value};
    check(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                    sizeof(inherited), nullptr, nullptr),
          "FOREGROUND_START", "Cannot restrict inherited handles");
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = input.value;
    si.StartupInfo.hStdOutput = output.value;
    si.StartupInfo.hStdError = output.value;
    si.lpAttributeList = attributes;
    PROCESS_INFORMATION pi{};
    auto command = L"\"" + helper.wstring() + L"\" --pipe";
    check(CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE,
                         EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
                         nullptr, nullptr, &si.StartupInfo, &pi),
          "FOREGROUND_START", "Cannot start foreground helper");
    Handle process(pi.hProcess), thread(pi.hThread);
    struct Reap
    {
        HANDLE process;
        ~Reap()
        {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
            {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 1000);
            }
        }
    } reap{process.value};
    check(AssignProcessToJobObject(job, process.value), "FOREGROUND_START", "Cannot supervise foreground helper");
    check(ResumeThread(thread.value) != DWORD(-1), "FOREGROUND_START", "Cannot resume foreground helper");
    input.reset();
    output.reset();
    wchar_t target[512]{};
    if (window)
        GetWindowTextW(window->hwnd, target, 512);
    auto status = state.status();
    auto request = json({{"server_pid", GetCurrentProcessId()},
                         {"server_process_created", process_created(GetCurrentProcessId())},
                         {"force_prompt", true},
                         {"session_consent", session},
                         {"timeout_ms", state.timeout_ms(session ? 45000 : 15000)},
                         {"language", status.value("language", "en")},
                         {"task_title", status.value("task_title", "")},
                         {"target_title", utf8(target)}}).dump() + "\n";
    DWORD count{};
    check(WriteFile(write.value, request.data(), DWORD(request.size()), &count, nullptr) &&
              count == request.size(), "FOREGROUND_PIPE", "Cannot send foreground request");
    write.reset();
    state.set_phase(session ? "waiting_consent" : "waiting_foreground");
    auto deadline = Clock::now() + std::chrono::milliseconds(state.timeout_ms(session ? 45000 : 15000));
    std::string buffer;
    while (Clock::now() < deadline)
    {
        if (state.cancelled(generation))
            throw Error("CANCELLED", "Permission request cancelled", session ? "consent" : "focus");
        if (!session)
            state.require_active();
        DWORD available{};
        if (!PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr))
            throw Error("FOREGROUND_HELPER_FAILED", "Foreground helper closed without a reply", "focus");
        if (available)
        {
            char bytes[2048];
            check(ReadFile(read.value, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr),
                  "FOREGROUND_PIPE", "Cannot read foreground reply");
            buffer.append(bytes, count);
            if (buffer.size() > 4096)
                throw Error("FOREGROUND_HELPER_FAILED", "Foreground reply is too large", "focus");
            if (buffer.find('\n') != std::string::npos)
            {
                auto response = json::parse(buffer.substr(0, buffer.find('\n')), nullptr, false);
                if (response.is_discarded() || !response.is_object())
                    throw Error("FOREGROUND_HELPER_FAILED", "Invalid foreground reply", "focus");
                if (!response.value("granted", false))
                {
                    const bool timed_out = response.value("reason", "") == "timeout";
                    throw Error(session ? (timed_out ? "CONSENT_TIMEOUT" : "CONSENT_DENIED") :
                                          (timed_out ? "FOREGROUND_TIMEOUT" : "FOCUS_DENIED"),
                                timed_out ? "Permission request timed out" :
                                (session ? "Desktop control permission was declined or unavailable" :
                                           "Foreground permission was declined or unavailable"),
                                session ? "consent" : "focus",
                                HRESULT_FROM_WIN32(timed_out ? DWORD(ERROR_TIMEOUT) :
                                                  response.value("win32_error", DWORD(ERROR_ACCESS_DENIED))));
                }
                if (state.cancelled(generation))
                    throw Error("CANCELLED", "Permission request cancelled", "consent");
                if (!session)
                    state.require_active();
                return;
            }
        }
        Sleep(5);
    }
    throw Error(session ? "CONSENT_TIMEOUT" : "FOREGROUND_TIMEOUT", "Permission request timed out", session ? "consent" : "focus");
}
void request_foreground_permission(State &state, HANDLE job, Window const &window)
{
    // Retain the legacy host API; state/job supervise the helper and window identifies the target.
    request_control_permission(state, job, &window);
}
void request_session_permission(State &state, HANDLE job, bool retry_consent)
{
    // Re-prompt only on an explicit retry; success remains valid for this connection.
    {
        std::lock_guard lock(state.mutex);
        if (state.session_authorized)
            return;
        if (state.session_consent_requested && !retry_consent)
            throw Error("CONSENT_RETRY_REQUIRED",
                        "Permission was not granted; call begin_task with retry_consent=true only after the user explicitly asks to retry",
                        "consent");
        state.session_consent_requested = true;
    }
    const auto generation = state.cancellation.load();
    request_control_permission(state, job, nullptr);
    std::lock_guard lock(state.mutex);
    if (state.cancelled(generation))
        throw Error("CANCELLED", "Session permission request cancelled", "consent");
    state.session_authorized = true;
}
} // namespace dc
