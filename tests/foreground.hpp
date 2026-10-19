// Controlled native MCP host regression: only its own fixture and host window
// receive synthetic mouse input. No claim of a physical human click test.
void pump()
{
    // Dispatch messages for the native test host; takes no parameters.
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
HWND find_grant_window()
{
    // Locate the legacy foreground dialog to assert it never appears; takes no parameters.
    HWND hwnd = FindWindowW(L"DesktopControlForegroundGrant", nullptr);
    return hwnd && IsWindowVisible(hwnd) ? hwnd : nullptr;
}
void test_click(HWND window, int x, int y)
{
    // Send input only to our fixture; window is the target, x/y are client pixels.
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(window);
    POINT point{x, y};
    ClientToScreen(window, &point);
    INPUT events[3]{};
    for (auto &event : events)
        event.type = INPUT_MOUSE;
    events[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    events[0].mi.dx = MulDiv(point.x - GetSystemMetrics(SM_XVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    events[0].mi.dy = MulDiv(point.y - GetSystemMetrics(SM_YVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    events[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    events[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SetLastError(0);
    auto inserted = SendInput(3, events, sizeof(INPUT));
    DWORD inputError = GetLastError(), foregroundPid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    record("test_click_delivery", {{"count", inserted}, {"win32_error", inputError},
                                    {"foreground_pid", foregroundPid}, {"test_host_pid", GetCurrentProcessId()}});
    ensure(inserted == 3, "test click inserted");
    for (int i = 0; i < 20; ++i)
    {
        pump();
        Sleep(5);
    }
}
void foreground_test(Client &client, std::filesystem::path directory, std::filesystem::path results)
{
    // Test cursor focus and session consent; client is MCP, directory holds fixtures, results stores evidence.
    auto launch = data(client.tool("launch_app", {{"app_id", utf8((directory / L"desktop-control-fixture.exe").wstring())}}));
    FixtureCleanup cleanup;
    cleanup.pid = launch["pid"];
    auto id = launch["windows"][0]["window_id"].get<std::string>();
    auto target = fixture_handle(cleanup.pid);
    auto info = data(client.tool("diagnostics"))["foreground_permission"];
    DWORD server = info["server_pid"];
    uint64_t created = std::stoull(info.at("server_process_created").get<std::string>());
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.lpszClassName = L"DesktopControlForegroundTestHost";
    ensure(RegisterClassW(&cls), "test host class");
    HWND host = CreateWindowExW(WS_EX_TOPMOST, cls.lpszClassName, L"Desktop Control foreground test host",
                                WS_CAPTION | WS_SYSMENU, 40, 340, 280, 160,
                                nullptr, nullptr, cls.hInstance, nullptr);
    ensure(host != nullptr, "test host window");
    struct Close
    {
        HWND hwnd;
        ~Close() { LockSetForegroundWindow(LSFW_UNLOCK); DestroyWindow(hwnd); }
    } close{host};
    ShowWindow(host, SW_SHOWNOACTIVATE);
    test_click(host, 100, 60);
    ensure(GetForegroundWindow() == host, "test host foreground");
    ensure(LockSetForegroundWindow(LSFW_LOCK), "lock test host foreground");
    auto denied = data(client.raw_tool("activate_window", {{"window_id", id}, {"cursor_fallback", false}}));
    ensure(denied["error"]["code"] == "FOCUS_DENIED" && denied["outcome"] == "rejected", "native focus refusal is explicit");
    ensure(denied["foreground_permission"]["server_pid"] == server, "denial identifies server");
    record("foreground_native_denial", denied);
    auto activated = data(client.tool("activate_window", {{"window_id", id}}));
    ensure(activated["focus_method"] == "title_bar_click", "cursor fallback used after native refusal");
    ensure(GetAncestor(GetForegroundWindow(), GA_ROOT) == target, "title-bar click activated target");
    ensure(find_grant_window() == nullptr, "no per-window permission dialog");
    ensure(client.consent_count() == 1, "only initial session permission");
    record("cursor_focus_fallback", {{"method", activated["focus_method"]}, {"consent_count", client.consent_count()}});

    // Foreground is lost again: prove there is still no new permission and input is not blindly sent.
    ensure(SendMessageW(target, WM_APP + 21, GetCurrentProcessId(), 0), "fixture returns focus eligibility");
    test_click(host, 100, 60);
    ensure(LockSetForegroundWindow(LSFW_LOCK), "lock foreground again");
    auto focusedOnly = data(client.tool("type_text", {{"window_id", id}, {"text", "must wait for fresh observation"}}));
    ensure(focusedOnly.value("focus_only", false) && !focusedOnly.value("action_performed", true), "activation does not send requested typing");
    ensure(focusedOnly.contains("observation") && find_grant_window() == nullptr, "fresh observation without permission dialog");
    ensure(find_element(data(observe(client, id)), 50004).value("value", "").empty(), "no typing during focus-only step");
    ensure(client.consent_count() == 1, "permission reused after focus loss");
    record("focus_only_before_input", {{"consent_count", client.consent_count()}});
    // The cursor click already moved foreground away from this host, so it no longer owns the lock.
    LockSetForegroundWindow(LSFW_UNLOCK);

    ensure(dc::foreground::grant(server, created + 1) == ERROR_INVALID_PARAMETER, "reject stale PID identity");
    ensure(dc::foreground::grant(ASFW_ANY, created) == ERROR_INVALID_PARAMETER, "reject global grant");
    auto obs = activated["observation"];
    auto edit = find_element(data(observe(client, id)), 50004);
    obs = data(observe(client, id));
    edit = find_element(obs, 50004);
    client.tool("click", coords(id, obs, edit));
    client.tool("press_key", {{"window_id", id}, {"key", "Ctrl+A"}});
    std::string value = "Foreground handoff: Привет! 😀";
    auto typed = data(client.tool("type_text", {{"window_id", id}, {"text", value}}));
    ensure(find_element(typed["observation"], 50004).value("value", "") == value, "post-handoff Unicode readback");
    save_image(observe(client, id), results / L"foreground-readback.png");
    record("foreground_input_verified", {{"value", value}});

    client.tool("close_window", {{"window_id", id}});
    client.tool("end_task");
    client.tool("begin_task", {{"title", "Second task in the same authorized connection"}});
    ensure(client.consent_count() == 1, "new task reuses connection permission");
    ensure(data(client.tool("diagnostics"))["state"]["session_authorized"].get<bool>(), "permission remains session-scoped");
    record("session_permission_reused", {{"count", client.consent_count()}});

    // A rejected connection never silently retries its permission dialog.
    Client deniedClient(directory / L"desktop-control.exe");
    deniedClient.call("initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", json::object()},
                                    {"clientInfo", {{"name", "consent-denial-test"}, {"version", "1"}}}});
    deniedClient.set_consent_response(false);
    auto refused = data(deniedClient.raw_tool("begin_task", {{"title", "Decline this test connection"}}));
    ensure(refused["error"]["code"] == "CONSENT_DENIED", "declined session rejects begin_task");
    deniedClient.set_consent_response(true);
    refused = data(deniedClient.raw_tool("begin_task", {{"title", "Do not prompt this connection again"}}));
    ensure(refused["error"]["code"] == "CONSENT_RETRY_REQUIRED", "declined consent requires explicit retry");
    ensure(deniedClient.consent_count() == 1, "exactly one prompt on declined connection");
    ensure(!data(deniedClient.tool("diagnostics"))["state"]["active"].get<bool>(), "declined connection cannot start control");
    record("session_permission_denial", {{"count", deniedClient.consent_count()}});
}
