#include "control.hpp"
#include <algorithm>
#include <d2d1.h>
#include <dwrite.h>
namespace dc
{
namespace
{
struct Overlay
{
    HWND hwnd{};
    bool panel{};
};
struct Ui
{
    State &state;
    std::vector<Overlay> blue, yellow;
    Overlay panel;
    winrt::com_ptr<ID2D1Factory> d2d;
    winrt::com_ptr<IDWriteFactory> write;
    HWND controller{};
    HHOOK keys{};
    HWINEVENTHOOK events{};
    bool active = false;
    Clock::time_point ended{};
    std::string previous;
    explicit Ui(State &s) : state(s)
    {
    }
};
thread_local Ui *current{};
thread_local unsigned expectedDestructions{};
void destroy_overlay(HWND hwnd)
{
    ++expectedDestructions;
    DestroyWindow(hwnd);
    --expectedDestructions;
}
LRESULT CALLBACK overlay_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (m == WM_LBUTTONUP && current)
    {
        RECT r{};
        GetClientRect(h, &r);
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        UINT dpi = GetDpiForWindow(h);
        if (x > r.right - MulDiv(130, dpi, 96) && y > r.bottom - MulDiv(42, dpi, 96))
        {
            current->state.stop();
            return 0;
        }
    }
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_PAINT)
    {
        PAINTSTRUCT p{};
        HDC destination = BeginPaint(h, &p);
        auto bitmap = reinterpret_cast<HBITMAP>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (bitmap)
        {
            HDC source = CreateCompatibleDC(destination);
            auto old = SelectObject(source, bitmap);
            RECT r{};
            GetClientRect(h, &r);
            BitBlt(destination, 0, 0, r.right, r.bottom, source, 0, 0, SRCCOPY);
            SelectObject(source, old);
            DeleteDC(source);
        }
        EndPaint(h, &p);
        return 0;
    }
    if (m == WM_NCDESTROY)
    {
        if (current && !expectedDestructions && !current->state.quitting)
        {
            current->state.stop();
            std::lock_guard lock(current->state.mutex);
            current->state.indicator_failed = true;
            current->state.ready = false;
            current->state.error = "Control indicator was unexpectedly destroyed";
            current->state.changed.notify_all();
        }
        auto bitmap = reinterpret_cast<HBITMAP>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (bitmap)
            DeleteObject(bitmap);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK controller_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_APP + 1 && current)
    {
        current->previous.clear();
        return 0;
    }
    if (m == WM_CLOSE)
    {
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK key_hook(int n, WPARAM w, LPARAM l)
{
    if (n >= 0 && current && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN))
    {
        auto k = reinterpret_cast<KBDLLHOOKSTRUCT *>(l);
        if (k->vkCode == VK_ESCAPE && !(k->flags & LLKHF_INJECTED) && k->dwExtraInfo != injection_tag)
        {
            auto s = current->state.status();
            if (s["active"].get<bool>())
                current->state.stop();
        }
    }
    return CallNextHookEx(nullptr, n, w, l);
}
void CALLBACK event_hook(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD)
{
    if (current)
    {
        if (event == EVENT_OBJECT_DESTROY && hwnd && object == OBJID_WINDOW && child == CHILDID_SELF)
        {
            note_window_destroyed(hwnd);
            std::lock_guard lock(current->state.mutex);
            if (current->state.target == hwnd)
            {
                current->state.target = nullptr;
                current->state.target_pid = 0;
                current->state.target_created = 0;
                if (current->state.active && !current->state.paused)
                    current->state.phase = "target_unavailable";
                current->state.client_phase.clear();
            }
        }
        PostMessageW(current->controller, WM_APP + 1, 0, 0);
    }
}
Overlay create_overlay(bool panel)
{
    DWORD styles = WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    if (!panel)
        styles |= WS_EX_TRANSPARENT;
    HWND h = CreateWindowExW(styles, L"DesktopControlOverlay", L"Desktop Control Indicator", WS_POPUP, 0, 0,
                             1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(h != nullptr, "INDICATOR_CREATE", "Cannot create overlay");
    check(SetLayeredWindowAttributes(h, 0, panel ? 245 : 255, LWA_ALPHA), "INDICATOR_ALPHA",
          "Layered alpha failed");
    return {h, panel};
}
void clear(std::vector<Overlay> &bars)
{
    for (auto b : bars)
        if (b.hwnd)
            destroy_overlay(b.hwnd);
    bars.clear();
}
void paint(Ui &ui, Overlay overlay, RECT r, D2D1_COLOR_F color, json const &state, UINT dpi)
{
    int width = r.right - r.left, height = r.bottom - r.top;
    if (width <= 0 || height <= 0)
    {
        ShowWindow(overlay.hwnd, SW_HIDE);
        return;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    void *pixels{};
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap)
    {
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        throw Error("INDICATOR_RENDER", "Cannot allocate indicator bitmap");
    }
    auto old = SelectObject(dc, bitmap);
    try
    {
        winrt::com_ptr<ID2D1DCRenderTarget> target;
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        winrt::check_hresult(ui.d2d->CreateDCRenderTarget(&props, target.put()));
        RECT bounds{0, 0, width, height};
        winrt::check_hresult(target->BindDC(dc, &bounds));
        target->BeginDraw();
        target->Clear(color);
        if (overlay.panel)
        {
            float scale = dpi / 96.0f;
            winrt::com_ptr<ID2D1SolidColorBrush> textBrush, buttonBrush;
            target->CreateSolidColorBrush(D2D1::ColorF(0xf5f7fa), textBrush.put());
            target->CreateSolidColorBrush(D2D1::ColorF(0xb42318), buttonBrush.put());
            winrt::com_ptr<IDWriteTextFormat> format;
            winrt::check_hresult(ui.write->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 14 * scale, L"", format.put()));
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            bool ru = state.value("language", "en") == "ru";
            auto phase = state.value("phase", "");
            std::string header =
                (phase == "completed" || phase == "disconnected")
                    ? localize(phase, ru)
                    : localize(state.value("paused", false) ? "paused_header" : "running", ru);
            std::string stage = localize(phase, ru);
            if (!state.value("detail", "").empty())
                stage += " — " + state["detail"].get<std::string>();
            HWND targetWindow = reinterpret_cast<HWND>(state.value("target", uint64_t(0)));
            if (targetWindow && (!IsWindow(targetWindow) || IsIconic(targetWindow)))
                stage = localize("target_unavailable", ru);
            std::vector<std::string> lines{header, state.value("task_title", ""), stage};
            for (size_t i = 0; i < lines.size(); ++i)
            {
                auto text = wide(lines[i]);
                float y = (12 + 26 * float(i)) * scale;
                target->DrawText(text.c_str(), (UINT32)text.size(), format.get(),
                                 D2D1::RectF(16 * scale, y, width - 16 * scale, y + 25 * scale),
                                 textBrush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            if (state.value("active", false))
            {
                auto button = D2D1::RoundedRect(D2D1::RectF(width - 125 * scale, height - 37 * scale,
                                                            width - 12 * scale, height - 8 * scale),
                                                5 * scale, 5 * scale);
                target->FillRoundedRectangle(button, buttonBrush.get());
                auto stop = wide(localize("stop", ru));
                target->DrawText(stop.c_str(), (UINT32)stop.size(), format.get(),
                                 D2D1::RectF(width - 116 * scale, height - 34 * scale, width - 16 * scale,
                                             height - 9 * scale),
                                 textBrush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        winrt::check_hresult(target->EndDraw());
        SelectObject(dc, old);
        auto previous = reinterpret_cast<HBITMAP>(
            SetWindowLongPtrW(overlay.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(bitmap)));
        bitmap = nullptr;
        if (previous)
            DeleteObject(previous);
        check(
            SetWindowPos(overlay.hwnd, nullptr, r.left, r.top, width, height, SWP_NOACTIVATE | SWP_NOZORDER),
            "INDICATOR_RENDER", "Overlay placement failed");
        RedrawWindow(overlay.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        ShowWindow(overlay.hwnd, SW_SHOWNOACTIVATE);
        winrt::check_hresult(DwmFlush());
        if (!SetWindowDisplayAffinity(overlay.hwnd, WDA_EXCLUDEFROMCAPTURE))
        {
            DWORD error = GetLastError();
            throw Error("INDICATOR_CAPTURE_EXCLUSION",
                        "Cannot exclude indicator from capture: Win32 " + std::to_string(error),
                        "SetWindowDisplayAffinity", HRESULT_FROM_WIN32(error));
        }
    }
    catch (...)
    {
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        throw;
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}
std::vector<RECT> strips(RECT r, int thickness)
{
    return {{r.left, r.top, r.right, r.top + thickness},
            {r.left, r.bottom - thickness, r.right, r.bottom},
            {r.left, r.top + thickness, r.left + thickness, r.bottom - thickness},
            {r.right - thickness, r.top + thickness, r.right, r.bottom - thickness}};
}
void render(Ui &ui)
{
    auto s = ui.state.status();
    HWND target;
    bool finishing;
    {
        std::lock_guard lock(ui.state.mutex);
        target = ui.state.target;
        finishing = ui.state.finish;
    }
    if (target)
    {
        DWORD pid{};
        GetWindowThreadProcessId(target, &pid);
        if (pid != s.value("target_pid", DWORD(0)) ||
            process_created(pid) != s.value("target_created", uint64_t(0)))
        {
            target = nullptr;
            s["detail"] = localize("target_unavailable", s.value("language", "en") == "ru");
        }
    }
    s["target"] = reinterpret_cast<uint64_t>(target);
    bool active = s.value("active", false);
    if (!active && !finishing)
    {
        clear(ui.blue);
        clear(ui.yellow);
        if (ui.panel.hwnd)
        {
            destroy_overlay(ui.panel.hwnd);
            ui.panel = {};
        }
        ui.active = false;
        ui.previous.clear();
        {
            std::lock_guard lock(ui.state.mutex);
            ui.state.overlays = json::object();
        }
        return;
    }
    if (finishing && !active)
    {
        if (ui.active)
        {
            ui.ended = Clock::now();
            ui.active = false;
        }
        clear(ui.blue);
        clear(ui.yellow);
        if (Clock::now() - ui.ended > std::chrono::seconds(2))
        {
            if (ui.panel.hwnd)
            {
                destroy_overlay(ui.panel.hwnd);
                ui.panel = {};
            }
            std::lock_guard lock(ui.state.mutex);
            ui.state.finish = false;
            return;
        }
    }
    if (active)
        ui.active = true;
    struct Monitor
    {
        RECT full, work;
    };
    std::vector<Monitor> monitors;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW(m, &mi))
                reinterpret_cast<std::vector<Monitor> *>(p)->push_back({mi.rcMonitor, mi.rcWork});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty())
        throw Error("INDICATOR_MONITOR", "No monitor is available");
    auto fingerprint = s.dump();
    for (auto m : monitors)
        fingerprint += rect_json(m.full).dump() + rect_json(m.work).dump();
    if (target && IsWindow(target))
        fingerprint += rect_json(window_rect(target)).dump() + std::to_string(IsIconic(target)) +
                       std::to_string(GetDpiForWindow(target));
    if (fingerprint == ui.previous)
        return;
    ui.previous = fingerprint;
    if (active)
    {
        size_t need = monitors.size() * 4;
        if (ui.blue.size() != need)
        {
            clear(ui.blue);
            for (size_t i = 0; i < need; ++i)
                ui.blue.push_back(create_overlay(false));
        }
        size_t i = 0;
        for (auto m : monitors)
        {
            UINT dpi = 96;
            HMONITOR mon = MonitorFromRect(&m.full, MONITOR_DEFAULTTONEAREST);
            using GetDpi = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
            static GetDpi getDpi = []() {
                HMODULE mod = LoadLibraryW(L"shcore.dll");
                return mod ? reinterpret_cast<GetDpi>(GetProcAddress(mod, "GetDpiForMonitor")) : nullptr;
            }();
            UINT dy{};
            if (getDpi)
                getDpi(mon, 0, &dpi, &dy);
            for (auto r : strips(m.full, std::max(1, MulDiv(3, dpi, 96))))
            {
                paint(ui, ui.blue[i], r, D2D1::ColorF(0x4da6ff), s, dpi);
                SetWindowPos(ui.blue[i++].hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }
    }
    MONITORINFO chosen{sizeof(chosen)};
    HMONITOR monitor = MonitorFromWindow(target ? target : ui.controller, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(monitor, &chosen);
    UINT dpi = target && IsWindow(target) ? GetDpiForWindow(target) : 96;
    if (!dpi)
        dpi = 96;
    if (active && target && IsWindowVisible(target) && !IsIconic(target))
    {
        if (ui.yellow.empty())
            for (int i = 0; i < 4; ++i)
                ui.yellow.push_back(create_overlay(false));
        auto r = window_rect(target);
        auto pieces = strips(r, std::max(1, MulDiv(1, dpi, 96)));
        HWND above = GetWindow(target, GW_HWNDPREV);
        while (above)
        {
            DWORD pid{};
            GetWindowThreadProcessId(above, &pid);
            if (pid != GetCurrentProcessId())
                break;
            above = GetWindow(above, GW_HWNDPREV);
        }
        for (size_t i = 0; i < 4; ++i)
        {
            paint(ui, ui.yellow[i], pieces[i], D2D1::ColorF(0xffd54f), s, dpi);
            SetWindowPos(ui.yellow[i].hwnd, above ? above : HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            above = ui.yellow[i].hwnd;
        }
    }
    else
        clear(ui.yellow);
    if (!ui.panel.hwnd)
        ui.panel = create_overlay(true);
    int width = std::min<int>(MulDiv(640, dpi, 96), chosen.rcWork.right - chosen.rcWork.left - 24);
    int height = MulDiv(126, dpi, 96);
    int left = chosen.rcWork.left + (chosen.rcWork.right - chosen.rcWork.left - width) / 2;
    RECT panel{left, chosen.rcWork.top + MulDiv(8, dpi, 96), left + width,
               chosen.rcWork.top + MulDiv(8, dpi, 96) + height};
    paint(ui, ui.panel, panel, D2D1::ColorF(0x17212b), s, dpi);
    SetWindowPos(ui.panel.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    json screens = json::array();
    for (auto m : monitors)
        screens.push_back(rect_json(m.full));
    {
        std::lock_guard lock(ui.state.mutex);
        ui.state.overlays = {{"screens", screens},
                             {"blue_strip_count", ui.blue.size()},
                             {"yellow_strip_count", ui.yellow.size()},
                             {"panel", rect_json(panel)},
                             {"capture_excluded", true}};
    }
    if (active)
    {
        winrt::check_hresult(DwmFlush());
        std::lock_guard lock(ui.state.mutex);
        ui.state.ready = true;
        ui.state.changed.notify_all();
    }
}
} // namespace
Indicator::Indicator(State &s) : state_(s)
{
    ULONG count{}, size{};
    GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size);
    std::vector<wchar_t> names(size + 1);
    if (size && GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names.data(), &size))
        state_.russian = _wcsnicmp(names.data(), L"ru", 2) == 0;
    thread_ = std::thread([this] { loop(); });
}
Indicator::~Indicator()
{
    state_.quitting = true;
    if (thread_.joinable())
        thread_.join();
}
void Indicator::start()
{
    // Start and wait for the task indicator using state_ timeout; takes no parameters.
    std::unique_lock lock(state_.mutex);
    if (state_.indicator_failed)
        throw Error("INDICATOR_UNAVAILABLE", state_.error);
    state_.ready = false;
    state_.active = true;
    state_.paused = false;
    state_.finish = false;
    state_.phase = "preparing";
    state_.error.clear();
    state_.changed.notify_all();
    if (!state_.changed.wait_for(lock, std::chrono::milliseconds(state_.timeout_ms(5000)),
                                 [&] { return state_.ready || state_.indicator_failed; }))
    {
        state_.active = false;
        throw Error("INDICATOR_TIMEOUT", "Visible indicator did not appear");
    }
    if (state_.indicator_failed)
    {
        state_.active = false;
        throw Error("INDICATOR_UNAVAILABLE", state_.error);
    }
}
void Indicator::finish()
{
    std::lock_guard lock(state_.mutex);
    state_.active = false;
    state_.ready = false;
    state_.paused = false;
    state_.finish = true;
    state_.phase = "completed";
    state_.target = nullptr;
    state_.target_pid = 0;
    state_.target_created = 0;
    state_.client_phase.clear();
    ++state_.cancellation;
}
void Indicator::loop()
{
    Ui ui(state_);
    current = &ui;
    try
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, ui.d2d.put()));
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown **>(ui.write.put())));
        WNDCLASSW overlay{};
        overlay.lpfnWndProc = overlay_proc;
        overlay.hInstance = GetModuleHandleW(nullptr);
        overlay.lpszClassName = L"DesktopControlOverlay";
        RegisterClassW(&overlay);
        WNDCLASSW controller{};
        controller.lpfnWndProc = controller_proc;
        controller.hInstance = GetModuleHandleW(nullptr);
        controller.lpszClassName = L"DesktopControlController";
        RegisterClassW(&controller);
        ui.controller = CreateWindowW(controller.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                      controller.hInstance, nullptr);
        check(ui.controller != nullptr, "INDICATOR_CREATE", "Indicator controller failed");
        ui.keys = SetWindowsHookExW(WH_KEYBOARD_LL, key_hook, GetModuleHandleW(nullptr), 0);
        check(ui.keys != nullptr, "INDICATOR_STOP_HOOK", "Physical Escape hook failed");
        ui.events = SetWinEventHook(EVENT_MIN, EVENT_MAX, nullptr, event_hook, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        check(ui.events != nullptr, "INDICATOR_EVENT_HOOK", "Window lifecycle hook failed");
        SetTimer(ui.controller, 1, 100, nullptr);
        while (!state_.quitting)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            try
            {
                render(ui);
            }
            catch (winrt::hresult_error const &e)
            {
                state_.stop();
                std::lock_guard lock(state_.mutex);
                state_.indicator_failed = true;
                state_.ready = false;
                state_.error = winrt::to_string(e.message());
                state_.changed.notify_all();
            }
            catch (std::exception const &e)
            {
                state_.stop();
                std::lock_guard lock(state_.mutex);
                state_.indicator_failed = true;
                state_.ready = false;
                state_.error = e.what();
                state_.changed.notify_all();
            }
            Sleep(50);
        }
    }
    catch (winrt::hresult_error const &e)
    {
        state_.stop();
        std::lock_guard lock(state_.mutex);
        state_.indicator_failed = true;
        state_.ready = false;
        state_.error = winrt::to_string(e.message());
        state_.changed.notify_all();
    }
    catch (std::exception const &e)
    {
        state_.stop();
        std::lock_guard lock(state_.mutex);
        state_.indicator_failed = true;
        state_.error = e.what();
        state_.changed.notify_all();
    }
    if (ui.keys)
        UnhookWindowsHookEx(ui.keys);
    if (ui.events)
        UnhookWinEvent(ui.events);
    clear(ui.blue);
    clear(ui.yellow);
    if (ui.panel.hwnd)
        destroy_overlay(ui.panel.hwnd);
    if (ui.controller)
        DestroyWindow(ui.controller);
    current = nullptr;
}
} // namespace dc
