#include "../resources/strings.hpp"
#include "control.hpp"
#include <sstream>
#include <winternl.h>
namespace dc
{
std::wstring wide(std::string const &s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (!n)
        throw Error("INVALID_UTF8", "Invalid UTF-8");
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), out.data(), n);
    return out;
}
std::string utf8(std::wstring const &s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}
std::string uuid()
{
    GUID g{};
    winrt::check_hresult(CoCreateGuid(&g));
    wchar_t text[40]{};
    StringFromGUID2(g, text, 40);
    return utf8(text);
}
std::wstring executable()
{
    std::wstring p(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, p.data(), (DWORD)p.size());
    p.resize(n);
    return p;
}
std::string base64(std::vector<uint8_t> const &data)
{
    static const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3)
    {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < data.size())
            v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < data.size())
            v |= data[i + 2];
        out += chars[(v >> 18) & 63];
        out += chars[(v >> 12) & 63];
        out += (i + 1 < data.size() ? chars[(v >> 6) & 63] : '=');
        out += (i + 2 < data.size() ? chars[v & 63] : '=');
    }
    return out;
}
json rect_json(RECT r)
{
    return {{"x", r.left}, {"y", r.top}, {"width", r.right - r.left}, {"height", r.bottom - r.top}};
}
RECT window_rect(HWND h)
{
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        check(GetWindowRect(h, &r), "WINDOW_GONE", "Cannot obtain window geometry");
    return r;
}
void check(bool ok, std::string const &code, std::string const &message)
{
    if (!ok)
        throw Error(code, message, "win32", HRESULT_FROM_WIN32(GetLastError()));
}
uint64_t process_created(DWORD pid)
{
    Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    FILETIME a{}, b{}, c{}, d{};
    if (!p || !GetProcessTimes(p.value, &a, &b, &c, &d))
        return 0;
    return (uint64_t(a.dwHighDateTime) << 32) | a.dwLowDateTime;
}
bool elevated(HWND h)
{
    DWORD pid{};
    GetWindowThreadProcessId(h, &pid);
    Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!p)
        return true;
    HANDLE raw{};
    if (!OpenProcessToken(p.value, TOKEN_QUERY, &raw))
        return true;
    Handle token(raw);
    DWORD len = 0;
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &len);
    std::vector<uint8_t> b(len);
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, b.data(), len, &len))
        return true;
    auto label = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(b.data());
    auto sid = label->Label.Sid;
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) > SECURITY_MANDATORY_MEDIUM_RID;
}
json os_version()
{
    using GetVersion = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
    auto function =
        reinterpret_cast<GetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOEXW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (!function || function(reinterpret_cast<PRTL_OSVERSIONINFOW>(&info)) < 0)
        return {{"supported", false}};
    return {{"major", info.dwMajorVersion},
            {"minor", info.dwMinorVersion},
            {"build", info.dwBuildNumber},
            {"workstation", info.wProductType == VER_NT_WORKSTATION},
            {"supported", info.dwMajorVersion >= 10 && info.dwBuildNumber >= 19045 &&
                              info.wProductType == VER_NT_WORKSTATION}};
}
bool input_desktop_available()
{
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!d)
        return false;
    wchar_t name[128]{};
    DWORD n = 0;
    bool ok =
        GetUserObjectInformationW(d, UOI_NAME, name, sizeof(name), &n) && _wcsicmp(name, L"Default") == 0;
    CloseDesktop(d);
    return ok;
}
void State::set_phase(std::string p, std::string d)
{
    std::lock_guard lock(mutex);
    if (!paused)
    {
        if (p != phase || !d.empty())
            detail = std::move(d);
        phase = std::move(p);
    }
    client_phase.clear();
    changed.notify_all();
}
void State::stop()
{
    std::lock_guard lock(mutex);
    if (!active)
    {
        ++cancellation;
        client_phase.clear();
        return;
    }
    paused = true;
    phase = "paused";
    client_phase.clear();
    ++cancellation;
    changed.notify_all();
}
void State::require_active() const
{
    std::lock_guard lock(mutex);
    if (!active)
        throw Error("TASK_REQUIRED", "Call begin_task first");
    if (paused)
        throw Error("PAUSED", "Call resume to continue");
    if (!ready || indicator_failed)
        throw Error("INDICATOR_UNAVAILABLE", "The visible indicator is unavailable");
    if (!input_desktop_available())
        throw Error("DESKTOP_UNAVAILABLE", "The interactive default desktop is unavailable");
}
bool State::cancelled(uint64_t g) const
{
    return quitting || cancellation.load() != g;
}
void State::set_target(Window const &w)
{
    std::lock_guard lock(mutex);
    target = w.hwnd;
    target_pid = w.pid;
    target_created = w.created;
}
json State::status() const
{
    // Return a locked snapshot of task and connection consent state; takes no parameters.
    std::lock_guard lock(mutex);
    auto p = phase;
    if (active && !paused && p == "waiting_ai" && !client_phase.empty() && Clock::now() < client_until)
        p = client_phase;
    return {{"active", active},
            {"session_authorized", session_authorized},
            {"session_consent_requested", session_consent_requested},
            {"paused", paused},
            {"indicator_ready", ready},
            {"indicator_failed", indicator_failed},
            {"task_id", task_id},
            {"task_title", title},
            {"phase", p},
            {"detail", detail},
            {"language", russian ? "ru" : "en"},
            {"error", error},
            {"target_pid", target_pid},
            {"target_created", target_created},
            {"overlays", overlays}};
}
std::string localize(std::string const &key, bool ru)
{
    auto i = resources::strings.find(key);
    return i == resources::strings.end() ? key : (ru ? i->second.second : i->second.first);
}
void worker_event(std::string const &phase)
{
    std::cout << json({{"event", "stage"}, {"phase", phase}}).dump() << '\n' << std::flush;
}
void Worker::reset()
{
    if (process_)
    {
        TerminateProcess(process_.value, 70);
        WaitForSingleObject(process_.value, 1000);
    }
    process_.reset();
    write_.reset();
    read_.reset();
    buffer_.clear();
}
void Worker::spawn()
{
    reset();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE childIn{}, parentOut{}, parentIn{}, childOut{};
    check(CreatePipe(&childIn, &parentOut, &sa, 65536), "WORKER_START", "CreatePipe failed");
    Handle ci(childIn);
    write_.reset(parentOut);
    check(CreatePipe(&parentIn, &childOut, &sa, 65536), "WORKER_START", "CreatePipe failed");
    Handle co(childOut);
    read_.reset(parentIn);
    SetHandleInformation(write_.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(read_.value, HANDLE_FLAG_INHERIT, 0);
    SIZE_T bytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<uint8_t> storage(bytes);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    check(InitializeProcThreadAttributeList(attributes, 1, 0, &bytes), "WORKER_START",
          "Attribute list failed");
    HANDLE handles[] = {ci.value, co.value};
    bool attrOk = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                            sizeof(handles), nullptr, nullptr);
    if (!attrOk)
    {
        DeleteProcThreadAttributeList(attributes);
        throw Error("WORKER_START", "Handle inheritance failed");
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = ci.value;
    si.StartupInfo.hStdOutput = co.value;
    si.StartupInfo.hStdError = co.value;
    si.lpAttributeList = attributes;
    PROCESS_INFORMATION pi{};
    auto cmd = L"\"" + executable() + L"\" --worker " + wide(mode_);
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attributes);
    check(ok, "WORKER_START", "CreateProcess failed");
    process_.reset(pi.hProcess);
    Handle thread(pi.hThread);
    check(AssignProcessToJobObject(job_, process_.value), "WORKER_START", "AssignProcessToJobObject failed");
    ResumeThread(thread.value);
}
json Worker::call(json request, unsigned timeout, bool mutation)
{
    // Wait for a worker; request is its payload, timeout the legacy budget, mutation marks possible side effects.
    timeout = state_.timeout_ms(timeout);
    if (state_.timeout_override_ms)
        request["timeout_ms"] = state_.timeout_override_ms;
    if (!process_ || WaitForSingleObject(process_.value, 0) == WAIT_OBJECT_0)
        spawn();
    auto generation = state_.cancellation.load();
    auto start = Clock::now();
    std::string stage = mode_;
    auto line = request.dump() + "\n";
    DWORD written{};
    if (!WriteFile(write_.value, line.data(), (DWORD)line.size(), &written, nullptr) ||
        written != line.size())
    {
        const auto hr = HRESULT_FROM_WIN32(GetLastError());
        reset();
        throw Error("WORKER_PIPE", "Worker request failed", stage, hr, mutation && written != 0);
    }
    for (;;)
    {
        if (state_.cancelled(generation))
        {
            reset();
            throw Error("CANCELLED", "Operation cancelled", stage, 0, mutation);
        }
        if (Clock::now() - start > std::chrono::milliseconds(timeout))
        {
            reset();
            throw Error("WORKER_TIMEOUT", "Worker exceeded its deadline", stage, 0, mutation);
        }
        DWORD available{};
        if (!PeekNamedPipe(read_.value, nullptr, 0, nullptr, &available, nullptr))
        {
            reset();
            throw Error("WORKER_EXITED", "Worker pipe closed", mode_, 0, mutation);
        }
        if (available)
        {
            char block[32768];
            DWORD n{};
            if (!ReadFile(read_.value, block, std::min<DWORD>(available, sizeof(block)), &n, nullptr))
            {
                const auto hr = HRESULT_FROM_WIN32(GetLastError());
                reset();
                throw Error("WORKER_PIPE", "Worker read failed", stage, hr, mutation);
            }
            buffer_.append(block, n);
            if (buffer_.size() > 96 * 1024 * 1024)
            {
                reset();
                throw Error("WORKER_LIMIT", "Worker response too large", stage, 0, mutation);
            }
        }
        size_t end;
        while ((end = buffer_.find('\n')) != std::string::npos)
        {
            json msg;
            try
            {
                msg = json::parse(buffer_.substr(0, end));
            }
            catch (...)
            {
                reset();
                throw Error("WORKER_PROTOCOL", "Invalid worker response", stage, 0, mutation);
            }
            buffer_.erase(0, end + 1);
            if (msg.value("event", "") == "stage")
            {
                stage = msg.value("phase", mode_);
                state_.set_phase(stage);
                continue;
            }
            if (msg.contains("error"))
            {
                auto e = msg["error"];
                auto reportedStage = e.value("stage", "");
                throw Error(e.value("code", "WORKER_ERROR"), e.value("message", "Worker failed"),
                            reportedStage.empty() ? stage : reportedStage, e.value("hresult", 0L),
                            e.value("unknown", mutation));
            }
            return msg.at("result");
        }
        if (WaitForSingleObject(process_.value, 0) == WAIT_OBJECT_0 && !available)
        {
            reset();
            throw Error("WORKER_EXITED", "Worker exited unexpectedly", mode_, 0, mutation);
        }
        Sleep(5);
    }
}
int worker_main(std::string const &mode)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::string line;
    while (std::getline(std::cin, line))
    {
        try
        {
            auto req = json::parse(line);
            if (req.value("operation", "") == "test_hang")
            {
                Sleep(INFINITE);
            }
            auto result = mode == "capture"  ? capture_request(req)
                          : mode == "launch" ? launch_request(req)
                                             : uia_request(req);
            std::cout << json({{"result", result}}).dump() << '\n' << std::flush;
        }
        catch (Error const &e)
        {
            std::cout << json({{"error",
                                {{"code", e.code},
                                 {"message", e.what()},
                                 {"stage", e.stage},
                                 {"hresult", e.hresult},
                                 {"unknown", e.unknown}}}})
                             .dump()
                      << '\n'
                      << std::flush;
        }
        catch (winrt::hresult_error const &e)
        {
            std::cout << json({{"error",
                                {{"code", "HRESULT"},
                                 {"message", winrt::to_string(e.message())},
                                 {"hresult", e.code().value}}}})
                             .dump()
                      << '\n'
                      << std::flush;
        }
        catch (std::exception const &e)
        {
            std::cout << json({{"error", {{"code", "WORKER_ERROR"}, {"message", e.what()}}}}).dump() << '\n'
                      << std::flush;
        }
    }
    return 0;
}
} // namespace dc
