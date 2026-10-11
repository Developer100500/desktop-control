#include <windows.h>
HWND editor;
WNDPROC originalEdit;
LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    return CallWindowProcW(originalEdit, h, m, w, l);
}
LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_APP + 21:
        // Test-only return handoff to the native MCP host, while this fixture
        // is foreground. Used to avoid relying on foreground-lock timeouts.
        if (GetForegroundWindow() == h && DWORD(w) && DWORD(w) != ASFW_ANY)
            return AllowSetForegroundWindow(DWORD(w));
        return FALSE;
    case WM_CREATE:
        editor = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, 20, 20,
                                 480, 120, h, (HMENU)101, GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"Test button", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 155, 140, 32, h,
                      (HMENU)102, GetModuleHandleW(nullptr), nullptr);
        originalEdit = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(editor, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(edit_proc)));
        SetFocus(editor);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == 102)
            SetWindowTextW(editor, L"Button invoked");
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        RECT rect{20, 215, 500, 440};
        FillRect(dc, &rect, (HBRUSH)GetStockObject(WHITE_BRUSH));
        Rectangle(dc, 20, 215, 500, 440);
        TextOutW(dc, 30, 230, L"Fixture canvas", 14);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW c{};
    c.hInstance = instance;
    c.lpfnWndProc = proc;
    c.lpszClassName = L"DesktopControlFixture";
    c.hCursor = LoadCursor(nullptr, IDC_ARROW);
    c.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&c);
    auto h = CreateWindowW(c.lpszClassName, L"Desktop Control Test Fixture", WS_OVERLAPPEDWINDOW, 100, 150,
                           550, 510, nullptr, nullptr, instance, nullptr);
    ShowWindow(h, show);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
