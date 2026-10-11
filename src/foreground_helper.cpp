#include <windows.h>
#include "json.hpp"
#include "foreground_permission.hpp"
#include "../resources/strings.hpp"
#include <string>

namespace
{
using json = nlohmann::json;
constexpr wchar_t window_class[] = L"DesktopControlForegroundGrant";
struct UI
{
    DWORD pid{};
    uint64_t created{};
    bool russian{}, done{}, granted{}, session{};
    DWORD error = ERROR_ACCESS_DENIED;
    std::wstring task, target;
};
std::wstring wide(std::string const &value)
{
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    if (size)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), result.data(), size);
    return result;
}
void label(HWND parent, wchar_t const *text, int x, int y, int width, int height)
{
    auto child = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, width, height,
                               parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(child, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}
std::wstring text(char const *key, bool russian)
{
    auto const &pair = dc::resources::strings.at(key);
    return wide(russian ? pair.second : pair.first);
}
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    // Handle consent UI events; hwnd is this helper window and the other parameters are Win32 event data.
    auto ui = reinterpret_cast<UI *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        ui = reinterpret_cast<UI *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }
    if (!ui)
        return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message)
    {
    case WM_CREATE:
    {
        label(hwnd, text(ui->session ? "session_request" : "foreground_request", ui->russian).c_str(), 20, 16, 470, 28);
        label(hwnd, ui->task.c_str(), 20, 48, 470, 32);
        label(hwnd, ui->target.c_str(), 20, 84, 470, 32);
        label(hwnd, text(ui->session ? "session_hint" : "foreground_hint", ui->russian).c_str(), 20, 124, 470, 28);
        auto button = CreateWindowW(L"BUTTON", text(ui->session ? "session_grant" : "foreground_grant", ui->russian).c_str(),
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                     20, 162, 220, 36, hwnd, HMENU(1), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(button, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        auto cancel = CreateWindowW(L"BUTTON", text("cancel", ui->russian).c_str(),
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                     264, 162, 220, 36, hwnd, HMENU(2), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(cancel, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SetTimer(hwnd, 1, 100, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == 1 && HIWORD(wparam) == BN_CLICKED)
        {
            // The user click supplies foreground eligibility. No synthetic Alt,
            // AttachThreadInput, or changes to global foreground lock policy.
            ui->error = dc::foreground::grant(ui->pid, ui->created);
            ui->granted = ui->session || ui->error == ERROR_SUCCESS;
            ui->done = true;
            return 0;
        }
        if (LOWORD(wparam) == 2)
        {
            ui->done = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        ui->done = true;
        return 0;
    case WM_TIMER:
    {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ui->pid);
        if (!process || dc::foreground::creation_time(process) != ui->created ||
            WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
            ui->done = true;
        if (process)
            CloseHandle(process);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
json run(json const &request, bool interactive)
{
    // Display the requested consent/handoff dialog; request carries server identity, interactive allows UI.
    UI ui;
    ui.pid = request.at("server_pid").get<DWORD>();
    ui.created = request.at("server_process_created").get<uint64_t>();
    ui.russian = request.value("language", "en") == "ru";
    ui.session = request.value("session_consent", false);
    ui.task = wide(request.value("task_title", ""));
    ui.target = wide(request.value("target_title", ""));
    // A successful grant does not guarantee activation (e.g. another process
    // has locked foreground). After the server's actual focus denial, require
    // an interactive handoff instead of treating an optimistic grant as proof.
    ui.error = request.value("force_prompt", false) && interactive
                   ? ERROR_ACCESS_DENIED : dc::foreground::grant(ui.pid, ui.created);
    if (ui.error == ERROR_SUCCESS)
        return {{"granted", true}, {"source", "eligible_caller"}, {"win32_error", 0}};
    if (!interactive || ui.error != ERROR_ACCESS_DENIED)
        return {{"granted", false}, {"win32_error", ui.error}};
    WNDCLASSW cls{};
    cls.lpfnWndProc = procedure;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.lpszClassName = ui.session ? L"DesktopControlSessionConsent" : window_class;
    if (!RegisterClassW(&cls))
        return {{"granted", false}, {"win32_error", GetLastError()}};
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, cls.lpszClassName,
                                text(ui.session ? "session_title" : "foreground_title", ui.russian).c_str(),
                                WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 520, 248,
                                nullptr, nullptr, cls.hInstance, &ui);
    if (!hwnd)
        return {{"granted", false}, {"win32_error", GetLastError()}};
    if (!SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA) ||
        !SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE))
    {
        DWORD error = GetLastError();
        DestroyWindow(hwnd);
        return {{"granted", false}, {"win32_error", error}};
    }
    SetPropW(hwnd, L"DesktopControlServerPid", reinterpret_cast<HANDLE>(ULONG_PTR(ui.pid)));
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    const auto timeout = request.value("timeout_ms", unsigned(ui.session ? 45000 : 15000));
    // Reply before the supervising deadline; request supplies the common budget in milliseconds.
    auto deadline = GetTickCount64() + (timeout > 500 ? timeout - 500 : timeout);
    while (!ui.done && GetTickCount64() < deadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE)
                ui.done = true;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    DestroyWindow(hwnd);
    return {{"granted", ui.granted}, {"source", "permission_window"}, {"win32_error", ui.error},
            {"reason", ui.done ? "completed" : "timeout"}};
}
} // namespace
int wmain(int argc, wchar_t **argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool pipe = argc == 2 && std::wstring(argv[1]) == L"--pipe";
    json response;
    try
    {
        json request;
        if (pipe)
        {
            std::string buffer;
            char bytes[2048];
            DWORD count{};
            while (buffer.find('\n') == std::string::npos)
            {
                if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), bytes, sizeof(bytes), &count, nullptr) || !count)
                    throw std::runtime_error("missing request");
                buffer.append(bytes, count);
                if (buffer.size() > 8192)
                    throw std::runtime_error("request too large");
            }
            request = json::parse(buffer.substr(0, buffer.find('\n')));
            response = run(request, true);
        }
        else if (argc == 4 && std::wstring(argv[1]) == L"--grant")
        {
            request = {{"server_pid", std::stoul(argv[2])},
                       {"server_process_created", std::stoull(argv[3])}};
            response = run(request, false);
        }
        else
            response = {{"granted", false}, {"win32_error", ERROR_INVALID_PARAMETER},
                        {"usage", "--grant SERVER_PID SERVER_PROCESS_CREATED (eligible caller only)"}};
    }
    catch (...)
    {
        response = {{"granted", false}, {"win32_error", ERROR_INVALID_DATA}};
    }
    auto line = response.dump() + "\n";
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), DWORD(line.size()), &written, nullptr);
    return response.value("granted", false) ? 0 : 1;
}
