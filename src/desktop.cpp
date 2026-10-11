#include "control.hpp"
#include <algorithm>
#include <cmath>
#include <propkey.h>
#include <shlobj.h>
#include <shobjidl.h>
namespace dc
{
namespace
{
std::mutex registrationMutex;
std::map<HWND, uint64_t> windowEpochs;
std::map<std::string, std::pair<HWND, uint64_t>> registeredEpochs;
void register_epoch(HWND hwnd, std::string const &id)
{
    std::lock_guard lock(registrationMutex);
    registeredEpochs[id] = {hwnd, windowEpochs[hwnd]};
}
bool registration_alive(HWND hwnd, std::string const &id)
{
    std::lock_guard lock(registrationMutex);
    auto found = registeredEpochs.find(id);
    return found != registeredEpochs.end() && found->second.first == hwnd &&
           found->second.second == windowEpochs[hwnd];
}
void forget_registration(std::string const &id)
{
    std::lock_guard lock(registrationMutex);
    auto found = registeredEpochs.find(id);
    if (found == registeredEpochs.end())
        return;
    auto hwnd = found->second.first;
    registeredEpochs.erase(found);
    if (std::none_of(registeredEpochs.begin(), registeredEpochs.end(),
                     [&](auto const &e) { return e.second.first == hwnd; }))
        windowEpochs.erase(hwnd);
}
std::wstring process_path(DWORD pid)
{
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    std::wstring path(32768, L'\0');
    DWORD n = (DWORD)path.size();
    if (!process || !QueryFullProcessImageNameW(process.value, 0, path.data(), &n))
        return {};
    path.resize(n);
    return path;
}
std::string title(HWND hwnd)
{
    int n = GetWindowTextLengthW(hwnd);
    std::wstring t(n + 1, L'\0');
    t.resize(GetWindowTextW(hwnd, t.data(), n + 1));
    return utf8(t);
}
json app_identity(HWND hwnd, DWORD pid)
{
    json out = {{"host_pid", pid}, {"application_pid", pid}};
    std::vector<DWORD> candidates{pid};
    EnumChildWindows(
        hwnd,
        [](HWND h, LPARAM data) -> BOOL {
            DWORD child{};
            GetWindowThreadProcessId(h, &child);
            auto &list = *reinterpret_cast<std::vector<DWORD> *>(data);
            if (std::find(list.begin(), list.end(), child) == list.end())
                list.push_back(child);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&candidates));
    for (DWORD p : candidates)
    {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p));
        if (!process)
            continue;
        UINT32 n{};
        if (GetApplicationUserModelId(process.value, &n, nullptr) == ERROR_INSUFFICIENT_BUFFER)
        {
            std::wstring aumid(n, L'\0');
            if (GetApplicationUserModelId(process.value, &n, aumid.data()) == ERROR_SUCCESS)
            {
                if (!aumid.empty() && aumid.back() == L'\0')
                    aumid.pop_back();
                out["aumid"] = utf8(aumid);
                out["application_pid"] = p;
                break;
            }
        }
    }
    return out;
}
INPUT key_input(WORD vk, DWORD flags = 0, WORD scan = 0)
{
    INPUT i{};
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = vk;
    i.ki.wScan = scan;
    i.ki.dwFlags = flags;
    if (vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP ||
        vk == VK_DOWN || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
        vk == VK_INSERT || vk == VK_DELETE)
        i.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    i.ki.dwExtraInfo = injection_tag;
    return i;
}
INPUT mouse_input(DWORD flags, LONG x = 0, LONG y = 0, DWORD data = 0)
{
    INPUT i{};
    i.type = INPUT_MOUSE;
    i.mi.dx = x;
    i.mi.dy = y;
    i.mi.dwFlags = flags;
    i.mi.mouseData = data;
    i.mi.dwExtraInfo = injection_tag;
    return i;
}
INPUT move_input(POINT p)
{
    int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN),
        width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 1 || height <= 1)
        throw Error("DISPLAY_GEOMETRY", "Invalid virtual screen");
    return mouse_input(MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE | MOUSEEVENTF_ABSOLUTE |
                           MOUSEEVENTF_VIRTUALDESK,
                       MulDiv(p.x - left, 65535, width - 1), MulDiv(p.y - top, 65535, height - 1));
}
WORD key_code(std::string name)
{
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    static const std::map<std::string, int> keys = {
        {"CTRL", VK_CONTROL},       {"CONTROL", VK_CONTROL}, {"CONTROL_L", VK_LCONTROL},
        {"CONTROL_R", VK_RCONTROL}, {"ALT", VK_MENU},        {"ALT_L", VK_LMENU},
        {"ALT_R", VK_RMENU},        {"SHIFT", VK_SHIFT},     {"SHIFT_L", VK_LSHIFT},
        {"SHIFT_R", VK_RSHIFT},     {"ENTER", VK_RETURN},    {"RETURN", VK_RETURN},
        {"ESCAPE", VK_ESCAPE},      {"ESC", VK_ESCAPE},      {"TAB", VK_TAB},
        {"SPACE", VK_SPACE},        {"BACKSPACE", VK_BACK},  {"DELETE", VK_DELETE},
        {"LEFT", VK_LEFT},          {"RIGHT", VK_RIGHT},     {"UP", VK_UP},
        {"DOWN", VK_DOWN},          {"HOME", VK_HOME},       {"END", VK_END},
        {"PAGEUP", VK_PRIOR},       {"PAGEDOWN", VK_NEXT},   {"INSERT", VK_INSERT},
        {"PERIOD", VK_OEM_PERIOD},  {"COMMA", VK_OEM_COMMA}, {"SLASH", VK_OEM_2},
        {"MINUS", VK_OEM_MINUS},    {"PLUS", VK_OEM_PLUS}};
    auto found = keys.find(name);
    if (found != keys.end())
        return WORD(found->second);
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
        return WORD(name[0]);
    if (name.size() > 1 && name[0] == 'F')
    {
        int value = 0;
        try
        {
            value = std::stoi(name.substr(1));
        }
        catch (...)
        {
        }
        if (value >= 1 && value <= 24)
            return WORD(VK_F1 + value - 1);
    }
    throw Error("INVALID_KEY", "Unsupported key name: " + name);
}
} // namespace
void note_window_destroyed(HWND hwnd)
{
    std::lock_guard lock(registrationMutex);
    if (auto found = windowEpochs.find(hwnd); found != windowEpochs.end())
        ++found->second;
}
json Desktop::describe(Window const &w)
{
    auto r = window_rect(w.hwnd);
    auto out = app_identity(w.hwnd, w.pid);
    out.update({{"window_id", w.id},
                {"title", title(w.hwnd)},
                {"pid", w.pid},
                {"process_path", utf8(process_path(w.pid))},
                {"visible", bool(IsWindowVisible(w.hwnd))},
                {"minimized", bool(IsIconic(w.hwnd))},
                {"geometry", rect_json(r)},
                {"dpi", GetDpiForWindow(w.hwnd)},
                {"elevated", elevated(w.hwnd)}});
    return out;
}
json Desktop::list_windows()
{
    std::vector<HWND> handles;
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            DWORD pid{};
            GetWindowThreadProcessId(h, &pid);
            if (!IsWindowVisible(h) || pid == GetCurrentProcessId())
                return TRUE;
            DWORD cloaked{};
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (cloaked)
                return TRUE;
            auto path = process_path(pid);
            if (path == executable())
                return TRUE;
            if (path == (std::filesystem::path(executable()).parent_path() / L"desktop-control-foreground.exe").wstring())
                return TRUE;
            reinterpret_cast<std::vector<HWND> *>(p)->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&handles));
    json out = json::array();
    std::set<std::string> live;
    for (HWND h : handles)
    {
        DWORD pid{};
        GetWindowThreadProcessId(h, &pid);
        auto created = process_created(pid);
        if (!created)
            continue;
        auto identity = app_identity(h, pid);
        DWORD appPid = identity.at("application_pid");
        auto appCreated = process_created(appPid);
        auto aumid = identity.value("aumid", "");
        auto found = std::find_if(windows_.begin(), windows_.end(), [&](auto const &entry) {
            auto const &w = entry.second;
            return w.hwnd == h && w.pid == pid && w.created == created && w.app_pid == appPid &&
                   w.app_created == appCreated && w.aumid == aumid && registration_alive(h, w.id);
        });
        std::string id;
        if (found == windows_.end())
        {
            id = uuid();
            windows_.emplace(id, Window{h, pid, created, id, appPid, appCreated, aumid});
            register_epoch(h, id);
        }
        else
            id = found->first;
        live.insert(id);
        try
        {
            out.push_back(describe(windows_.at(id)));
        }
        catch (...)
        {
        }
    }
    for (auto it = windows_.begin(); it != windows_.end();)
        if (!live.contains(it->first))
        {
            forget_registration(it->first);
            it = windows_.erase(it);
        }
        else
            ++it;
    return out;
}
Window Desktop::resolve(std::string const &id)
{
    auto it = windows_.find(id);
    if (it == windows_.end())
        throw Error("UNKNOWN_WINDOW", "Select a window from list_windows");
    auto w = it->second;
    DWORD pid{};
    GetWindowThreadProcessId(w.hwnd, &pid);
    if (!IsWindow(w.hwnd) || pid != w.pid || process_created(pid) != w.created ||
        !registration_alive(w.hwnd, w.id))
        throw Error("WINDOW_GONE", "Window identity changed");
    auto identity = app_identity(w.hwnd, w.pid);
    if (identity["application_pid"].get<DWORD>() != w.app_pid ||
        process_created(w.app_pid) != w.app_created || identity.value("aumid", "") != w.aumid)
        throw Error("WINDOW_GONE", "Packaged application identity changed");
    if (elevated(w.hwnd))
        throw Error("ELEVATED_TARGET", "Elevated applications are excluded");
    return w;
}
json Desktop::list_apps()
{
    json apps = json::array();
    std::set<std::string> ids;
    winrt::com_ptr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, nullptr, IID_PPV_ARGS(folder.put()))))
    {
        winrt::com_ptr<IEnumShellItems> enumeration;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(enumeration.put()))))
        {
            for (;;)
            {
                winrt::com_ptr<IShellItem> item;
                ULONG fetched{};
                if (enumeration->Next(1, item.put(), &fetched) != S_OK)
                    break;
                winrt::com_ptr<IShellItem2> properties;
                if (FAILED(item->QueryInterface(IID_PPV_ARGS(properties.put()))))
                    continue;
                PWSTR name{}, id{}, path{};
                item->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
                properties->GetString(PKEY_AppUserModel_ID, &id);
                properties->GetString(PKEY_Link_TargetParsingPath, &path);
                std::string aid = id ? utf8(id) : "";
                std::wstring file = path ? path : L"";
                if (!aid.empty() && aid.find('!') != std::string::npos)
                {
                    apps.push_back({{"app_id", aid},
                                    {"name", name ? utf8(name) : aid},
                                    {"kind", "packaged"},
                                    {"aumid", aid}});
                    ids.insert(aid);
                }
                else if (!file.empty() && std::filesystem::path(file).extension() == L".exe")
                {
                    auto key = utf8(file);
                    apps.push_back({{"app_id", key},
                                    {"name", name ? utf8(name) : key},
                                    {"kind", "win32"},
                                    {"path", key}});
                    ids.insert(key);
                }
                CoTaskMemFree(name);
                CoTaskMemFree(id);
                CoTaskMemFree(path);
            }
        }
    }
    for (auto const &w : list_windows())
    {
        auto path = w.value("process_path", "");
        if (!path.empty() && ids.insert(path).second)
            apps.push_back(
                {{"app_id", path}, {"name", w.value("title", path)}, {"kind", "win32"}, {"path", path}});
    }
    return apps;
}
json launch_request(json const &args)
{
    worker_event("launch_app");
    if (args.contains("arguments") && args.at("arguments").size() > 64)
        throw Error("ARGUMENT_LIMIT", "Use at most 64 launch arguments");
    DWORD pid{};
    auto app = args.at("app_id").get<std::string>();
    if (app.find('!') != std::string::npos && !std::filesystem::path(wide(app)).is_absolute())
    {
        if (args.contains("arguments") && !args.at("arguments").empty())
            throw Error("ARGUMENT_UNSUPPORTED",
                        "Packaged launch arguments are not supported in this release");
        winrt::com_ptr<IApplicationActivationManager> manager;
        winrt::check_hresult(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(manager.put())));
        winrt::check_hresult(manager->ActivateApplication(wide(app).c_str(), nullptr, AO_NONE, &pid));
    }
    else
    {
        auto path = wide(app);
        if (!std::filesystem::path(path).is_absolute() ||
            std::filesystem::path(path).extension() != L".exe" || !std::filesystem::is_regular_file(path))
            throw Error("INVALID_APP", "Use an absolute existing .exe path or an AUMID");
        auto command = L"\"" + path + L"\"";
        if (args.contains("arguments"))
        {
            for (auto const &arg : args.at("arguments"))
            {
                auto value = wide(arg.get<std::string>());
                std::wstring quoted = L"\"";
                size_t slash = 0;
                for (wchar_t c : value)
                {
                    if (c == L'\\')
                    {
                        ++slash;
                        continue;
                    }
                    if (c == L'"')
                    {
                        quoted.append(slash * 2 + 1, L'\\');
                        quoted += c;
                    }
                    else
                    {
                        quoted.append(slash, L'\\');
                        quoted += c;
                    }
                    slash = 0;
                }
                quoted.append(slash * 2, L'\\');
                quoted += L'"';
                command += L" " + quoted;
            }
        }
        if (command.size() > 32760)
            throw Error("ARGUMENT_LIMIT", "Launch command line is too long");
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        check(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB,
                             nullptr, nullptr, &si, &pi),
              "APP_LAUNCH", "Application launch failed");
        Handle process(pi.hProcess), thread(pi.hThread);
        pid = pi.dwProcessId;
    }
    return {{"pid", pid}};
}
json Desktop::launch(json const &args, Worker &worker)
{
    // Launch args through worker and wait for a target window using the configured budget.
    state_.require_active();
    state_.set_phase("launch_app");
    auto started = Clock::now();
    auto launched = worker.call(args, 15000, true);
    DWORD pid = launched.at("pid");
    auto app = args.at("app_id").get<std::string>();
    auto generation = state_.cancellation.load();
    auto start = started;
    while (Clock::now() - start < std::chrono::milliseconds(state_.timeout_ms(15000)))
    {
        if (state_.cancelled(generation))
            throw Error("CANCELLED", "Launch wait cancelled", "launch_app", 0, true);
        auto windows = list_windows();
        json matches = json::array();
        for (auto const &w : windows)
            if (w["application_pid"] == pid || w["pid"] == pid || w.value("aumid", "") == app)
                matches.push_back(w);
        if (!matches.empty())
            return {{"pid", pid}, {"windows", matches}, {"outcome", "delivered"}};
        Sleep(50);
    }
    return {{"pid", pid},
            {"windows", json::array()},
            {"outcome", "delivered"},
            {"warning", "No targetable window appeared within " + std::to_string(state_.timeout_ms(15000)) + " ms"}};
}
json Desktop::focus(Window const &w, bool cursor, json const &activation)
{
    // Activate w with native focus or a cursor click; cursor enables fallback, activation carries an optional observed point.
    state_.require_active();
    if (!IsWindowVisible(w.hwnd))
        throw Error("WINDOW_HIDDEN", "Target is not visible", "focus");
    if (IsIconic(w.hwnd))
        throw Error("WINDOW_MINIMIZED", "Restore target before interacting", "focus");
    state_.set_target(w);
    const auto generation = state_.cancellation.load();
    const auto root = GetAncestor(w.hwnd, GA_ROOT);
    auto focused = [&] {
        // Check whether the selected root owns foreground; captures root and takes no parameters.
        return GetAncestor(GetForegroundWindow(), GA_ROOT) == root;
    };
    bool supplied = activation.contains("x") || activation.contains("y") || activation.contains("observation_id");
    if (supplied)
    {
        if (!activation.contains("x") || !activation.contains("y") || !activation.contains("observation_id"))
            throw Error("INVALID_ARGUMENT", "Activation coordinates require x, y and observation_id together");
        observation(activation);
    }
    if (focused())
        return {{"focus_method", "already_foreground"}, {"activation_click", false}};
    SetForegroundWindow(w.hwnd);
    for (int i = 0; i < 20 && !focused(); ++i)
    {
        if (state_.cancelled(generation))
            throw Error("CANCELLED", "Activation cancelled", "focus");
        Sleep(10);
    }
    if (focused())
        return {{"focus_method", "native"}, {"activation_click", false}};
    if (!cursor)
        throw Error("FOCUS_DENIED", "Windows denied native activation; enable cursor_fallback", "focus");
    for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000)
            throw Error("USER_INPUT_HELD", "Release held mouse buttons or modifier keys before activation", "focus");
    resolve(w.id);
    state_.require_active();
    // Raise the target without assuming it gained focus. Never change topmost status.
    SetWindowPos(w.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Sleep(30);
    POINT click{};
    bool found = false;
    if (supplied)
    {
        click = point(activation, w);
        found = true;
    }
    else
    {
        const auto rect = window_rect(w.hwnd);
        const auto deadline = Clock::now() + std::chrono::milliseconds(state_.timeout_ms(800));
        for (LONG y = rect.top + 6; y < std::min(rect.bottom, rect.top + LONG(96)) && !found; y += 8)
            for (int fraction : {5, 4, 6, 3, 7, 2, 8})
            {
                if (Clock::now() >= deadline)
                    break;
                if (state_.cancelled(generation))
                    throw Error("CANCELLED", "Activation cancelled before click", "focus");
                POINT candidate{rect.left + (rect.right - rect.left) * fraction / 10, y};
                if (GetAncestor(WindowFromPoint(candidate), GA_ROOT) != root)
                    continue;
                DWORD_PTR hit{};
                if (SendMessageTimeoutW(w.hwnd, WM_NCHITTEST, 0,
                                        MAKELPARAM(short(candidate.x), short(candidate.y)),
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 20, &hit) && hit == HTCAPTION)
                {
                    click = candidate;
                    found = true;
                    break;
                }
            }
    }
    if (!found)
        throw Error("FOCUS_POINT_REQUIRED", "No unobstructed title-bar point; observe and call activate_window with observation_id and safe x/y", "focus");
    resolve(w.id);
    state_.require_active();
    if (state_.cancelled(generation))
        throw Error("CANCELLED", "Activation cancelled before click", "focus");
    if (GetAncestor(WindowFromPoint(click), GA_ROOT) != root)
        throw Error("POINT_OCCLUDED", "Activation point now belongs to another window", "focus");
    bool delivered = false;
    try
    {
        send({move_input(click), mouse_input(MOUSEEVENTF_LEFTDOWN), mouse_input(MOUSEEVENTF_LEFTUP)});
        delivered = true;
        for (int i = 0; i < 50 && !focused(); ++i)
        {
            if (state_.cancelled(generation))
                throw Error("CANCELLED", "Activation cancelled after click", "focus", 0, true);
            Sleep(10);
        }
        if (!focused())
            throw Error("FOCUS_DENIED", "The activation click was delivered but the target did not gain focus", "focus", 0, true);
    }
    catch (Error &error)
    {
        error.unknown = error.unknown || delivered;
        release_input();
        invalidate();
        throw;
    }
    return {{"focus_method", supplied ? "observed_point_click" : "title_bar_click"},
            {"activation_click", true}, {"activation_point_screen", {{"x", click.x}, {"y", click.y}}}};
}
void Desktop::send(std::vector<INPUT> const &inputs)
{
    state_.require_active();
    if (inputs.empty())
        return;
    std::lock_guard guard(input_mutex_);
    state_.require_active();
    UINT count = SendInput((UINT)inputs.size(), const_cast<INPUT *>(inputs.data()), sizeof(INPUT));
    const DWORD deliveryError = GetLastError();
    // Only events actually inserted by SendInput become owned input state.
    for (size_t n = 0; n < count; ++n)
    {
        auto const &i = inputs[n];
        if (i.type == INPUT_KEYBOARD && !(i.ki.dwFlags & KEYEVENTF_KEYUP))
        {
            if (i.ki.dwFlags & KEYEVENTF_UNICODE)
                pressed_unicode_.insert(i.ki.wScan);
            else
                pressed_keys_.insert(i.ki.wVk);
        }
        if (i.type == INPUT_MOUSE)
        {
            if (i.mi.dwFlags & MOUSEEVENTF_LEFTDOWN)
                pressed_buttons_.insert(MOUSEEVENTF_LEFTUP);
            if (i.mi.dwFlags & MOUSEEVENTF_RIGHTDOWN)
                pressed_buttons_.insert(MOUSEEVENTF_RIGHTUP);
            if (i.mi.dwFlags & MOUSEEVENTF_MIDDLEDOWN)
                pressed_buttons_.insert(MOUSEEVENTF_MIDDLEUP);
        }
        if (i.type == INPUT_KEYBOARD && (i.ki.dwFlags & KEYEVENTF_KEYUP))
        {
            if (i.ki.dwFlags & KEYEVENTF_UNICODE)
                pressed_unicode_.erase(i.ki.wScan);
            else
                pressed_keys_.erase(i.ki.wVk);
        }
        if (i.type == INPUT_MOUSE)
        {
            if (i.mi.dwFlags & MOUSEEVENTF_LEFTUP)
                pressed_buttons_.erase(MOUSEEVENTF_LEFTUP);
            if (i.mi.dwFlags & MOUSEEVENTF_RIGHTUP)
                pressed_buttons_.erase(MOUSEEVENTF_RIGHTUP);
            if (i.mi.dwFlags & MOUSEEVENTF_MIDDLEUP)
                pressed_buttons_.erase(MOUSEEVENTF_MIDDLEUP);
        }
    }
    if (count != inputs.size())
        throw Error("INPUT_PARTIAL", "Input delivery was incomplete", "input",
                    HRESULT_FROM_WIN32(deliveryError), count != 0);
}
void Desktop::release_input()
{
    std::lock_guard guard(input_mutex_);
    std::vector<INPUT> release;
    for (WORD k : pressed_keys_)
        release.push_back(key_input(k, KEYEVENTF_KEYUP));
    for (WORD c : pressed_unicode_)
        release.push_back(key_input(0, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, c));
    for (DWORD up : pressed_buttons_)
        release.push_back(mouse_input(up));
    pressed_keys_.clear();
    pressed_unicode_.clear();
    pressed_buttons_.clear();
    if (!release.empty())
        SendInput((UINT)release.size(), release.data(), sizeof(INPUT));
}
bool Desktop::image_snapshot(std::string const &id)
{
    auto found = observations_.find(id);
    if (found == observations_.end() || !found->second.contains("image"))
        return false;
    try
    {
        observation({{"observation_id", id}, {"window_id", found->second.at("window_id")}});
        return true;
    }
    catch (...)
    {
        return false;
    }
}
void Desktop::invalidate()
{
    observations_.clear();
}
void Desktop::remember(json const &obs)
{
    for (auto it = observations_.begin(); it != observations_.end();)
        if (it->second["window_id"] == obs["window_id"])
            it = observations_.erase(it);
        else
            ++it;
    observations_[obs.at("observation_id")] = obs;
    // Coordinate/UIA validation needs metadata, never another retained copy of the PNG.
    auto &saved = observations_[obs.at("observation_id")];
    if (saved.contains("image"))
        saved["image"].erase("base64");
}
json Desktop::observation(json const &args)
{
    // Validate the snapshot/window identifiers in args using the configured lifetime.
    auto id = args.at("observation_id").get<std::string>();
    auto found = observations_.find(id);
    if (found == observations_.end())
        throw Error("STALE_OBSERVATION", "Observe the window again");
    auto obs = found->second;
    auto age =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() -
        obs.at("steady_ms").get<int64_t>();
    if (age > state_.timeout_ms(60000))
        throw Error("STALE_OBSERVATION", "Observation expired after " +
                    std::to_string(state_.timeout_ms(60000)) + " ms");
    auto w = resolve(args.at("window_id"));
    if (obs["window_id"] != w.id || obs["geometry"] != rect_json(window_rect(w.hwnd)))
        throw Error("GEOMETRY_CHANGED", "Window moved or resized; observe again");
    return obs;
}
POINT Desktop::point(json const &args, Window const &w)
{
    auto obs = observation(args);
    if (!obs.value("coordinate_mapping_valid", false))
        throw Error("GEOMETRY_UNVERIFIED", "No verified screenshot coordinate mapping");
    double x = args.at("x").get<double>(), y = args.at("y").get<double>();
    auto r = window_rect(w.hwnd);
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= r.right - r.left ||
        y >= r.bottom - r.top)
        throw Error("POINT_OUTSIDE", "Point is outside target image");
    POINT p{r.left + LONG(std::floor(x)), r.top + LONG(std::floor(y))};
    HWND hit = WindowFromPoint(p);
    if (GetAncestor(hit, GA_ROOT) != GetAncestor(w.hwnd, GA_ROOT))
        throw Error("POINT_OCCLUDED", "Point belongs to another window");
    return p;
}
json Desktop::act(std::string const &tool, json const &args)
{
    // Perform tool on its identified window; args supplies validated inputs and any observation identifiers.
    state_.require_active();
    auto w = resolve(args.at("window_id"));
    auto generation = state_.cancellation.load();
    state_.set_phase(tool, tool == "press_key" ? args.value("key", "") : "");
    if (tool == "click" || tool == "scroll" || tool == "drag")
        observation(args);
    json focusResult = json::object();
    if (tool == "close_window")
        state_.set_target(w);
    else
        focusResult = focus(w, args.value("cursor_fallback", true),
                            tool == "activate_window" ? args : json::object());
    if (focusResult.value("activation_click", false) && tool != "activate_window")
    {
        // Activation is a separate visible action: return its new observation before sending requested input.
        invalidate();
        focusResult.update({{"outcome", "delivered"}, {"window_id", w.id},
                            {"focus_only", true}, {"action_performed", false},
                            {"retry_after_observation", true}});
        return focusResult;
    }
    if (state_.cancelled(generation))
        throw Error("CANCELLED", "Action cancelled before input", "focus");
    bool delivered = false;
    auto inject = [&](std::vector<INPUT> const &inputs) {
        send(inputs);
        delivered = true;
    };
    try
    {
        if (tool == "activate_window")
        {
        }
        else if (tool == "close_window")
            check(PostMessageW(w.hwnd, WM_CLOSE, 0, 0), "CLOSE_FAILED", "Cannot request window close");
        else if (tool == "click")
        {
            auto p = point(args, w);
            int count = args.value("count", 1);
            if (count < 1 || count > 2)
                throw Error("INVALID_COUNT", "Click count must be 1 or 2");
            std::string button = args.value("button", "left");
            DWORD down = button == "left"    ? MOUSEEVENTF_LEFTDOWN
                         : button == "right" ? MOUSEEVENTF_RIGHTDOWN
                                             : MOUSEEVENTF_MIDDLEDOWN;
            DWORD up = button == "left"    ? MOUSEEVENTF_LEFTUP
                       : button == "right" ? MOUSEEVENTF_RIGHTUP
                                           : MOUSEEVENTF_MIDDLEUP;
            if (GetAsyncKeyState(button == "left"    ? VK_LBUTTON
                                 : button == "right" ? VK_RBUTTON
                                                     : VK_MBUTTON) &
                0x8000)
                throw Error("USER_BUTTON_HELD", "Release the mouse button first");
            inject({move_input(p)});
            for (int i = 0; i < count; ++i)
            {
                inject({mouse_input(down), mouse_input(up)});
                if (i + 1 < count)
                    Sleep(40);
            }
        }
        else if (tool == "scroll")
        {
            auto p = point(args, w);
            int vertical = args.value("vertical", 0), horizontal = args.value("horizontal", 0);
            if (std::abs(int64_t(vertical)) > 12000 || std::abs(int64_t(horizontal)) > 12000)
                throw Error("SCROLL_LIMIT", "Wheel delta exceeds limit");
            inject({move_input(p)});
            if (vertical)
                inject({mouse_input(MOUSEEVENTF_WHEEL, 0, 0, DWORD(vertical))});
            if (horizontal)
                inject({mouse_input(MOUSEEVENTF_HWHEEL, 0, 0, DWORD(horizontal))});
        }
        else if (tool == "drag")
        {
            auto points = args.at("points");
            int duration = args.value("duration_ms", 500);
            if (!points.is_array() || points.size() < 2 || points.size() > 1000 || duration < 1 ||
                duration > 5000)
                throw Error("INVALID_DRAG", "Drag needs 2..1000 points and 1..5000 ms");
            std::vector<POINT> path;
            for (auto const &item : points)
            {
                if (state_.cancelled(generation))
                    throw Error("CANCELLED", "Drag preparation cancelled", "drag");
                auto p = args;
                p["x"] = item.at("x");
                p["y"] = item.at("y");
                path.push_back(point(p, w));
            }
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
                throw Error("USER_BUTTON_HELD", "Release the mouse button first");
            inject({move_input(path.front()), mouse_input(MOUSEEVENTF_LEFTDOWN)});
            auto start = Clock::now();
            for (size_t i = 1; i < path.size(); ++i)
            {
                auto due = start + std::chrono::milliseconds(duration * int(i) / int(path.size() - 1));
                while (Clock::now() < due)
                {
                    if (state_.cancelled(generation))
                        throw Error("CANCELLED", "Drag cancelled", "drag", 0, true);
                    Sleep(5);
                }
                if (GetForegroundWindow() != w.hwnd)
                    throw Error("FOCUS_LOST", "Focus changed during drag", "drag", 0, true);
                observation(args);
                if (Clock::now() - start >= std::chrono::seconds(5))
                    throw Error("DRAG_TIMEOUT", "Drag exceeded the five-second wall-clock limit", "drag", 0,
                                true);
                if (GetAncestor(WindowFromPoint(path[i]), GA_ROOT) != GetAncestor(w.hwnd, GA_ROOT))
                    throw Error("POINT_OCCLUDED", "Drag path became obstructed", "drag", 0, true);
                inject({move_input(path[i])});
            }
            inject({mouse_input(MOUSEEVENTF_LEFTUP)});
        }
        else if (tool == "press_key")
        {
            std::vector<WORD> keys;
            if (args.contains("keys"))
            {
                for (auto const &k : args.at("keys"))
                    keys.push_back(key_code(k));
            }
            else
            {
                std::string chord = args.at("key");
                size_t pos = 0;
                for (;;)
                {
                    auto end = chord.find('+', pos);
                    keys.push_back(key_code(chord.substr(pos, end - pos)));
                    if (end == std::string::npos)
                        break;
                    pos = end + 1;
                }
            }
            if (keys.empty() || keys.size() > 8)
                throw Error("INVALID_KEY", "Use 1..8 keys");
            for (WORD k : keys)
                if (GetAsyncKeyState(k) & 0x8000)
                    throw Error("USER_KEY_HELD", "A requested key is already pressed by the user");
            for (WORD k : keys)
                inject({key_input(k)});
            for (auto i = keys.rbegin(); i != keys.rend(); ++i)
                inject({key_input(*i, KEYEVENTF_KEYUP)});
        }
        else if (tool == "type_text")
        {
            auto text = wide(args.at("text"));
            if (text.size() > 32768)
                throw Error("TEXT_LIMIT", "Text exceeds 32768 UTF-16 units");
            for (WORD k : {WORD(VK_CONTROL), WORD(VK_MENU), WORD(VK_SHIFT)})
                if (GetAsyncKeyState(k) & 0x8000)
                    throw Error("USER_KEY_HELD", "Release modifier keys before typing");
            for (wchar_t c : text)
            {
                if (state_.cancelled(generation))
                    throw Error("CANCELLED", "Typing cancelled", "type_text", 0, true);
                if (GetForegroundWindow() != w.hwnd)
                    throw Error("FOCUS_LOST", "Focus changed while typing", "type_text", 0, true);
                inject({key_input(0, KEYEVENTF_UNICODE, c),
                        key_input(0, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, c)});
            }
        }
        else
            throw Error("UNKNOWN_TOOL", "Unknown desktop action");
    }
    catch (Error &e)
    {
        e.unknown = e.unknown || delivered;
        release_input();
        invalidate();
        throw;
    }
    catch (...)
    {
        release_input();
        invalidate();
        if (delivered)
            throw Error("ACTION_INTERRUPTED", "Action interrupted after input delivery", tool, 0, true);
        throw;
    }
    release_input();
    invalidate();
    focusResult.update({{"outcome", "delivered"}, {"window_id", w.id}, {"action_performed", true}});
    return focusResult;
}
} // namespace dc
