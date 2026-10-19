#include "json.hpp"
#include "foreground_permission.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
std::ofstream logFile;
std::mutex logMutex;
void record(std::string const &name, json detail = json::object())
{
    std::lock_guard lock(logMutex);
    logFile << json({{"test", name}, {"detail", detail}}).dump() << '\n' << std::flush;
}
void ensure(bool good, std::string message)
{
    if (!good)
        throw std::runtime_error(message);
}
struct H
{
    HANDLE h{};
    H() = default;
    H(HANDLE value) : h(value)
    {
    }
    ~H()
    {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    H(H const &) = delete;
};
std::string utf8(std::wstring s)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), size, nullptr, nullptr);
    return out;
}
class Client
{
    H input, output, process;
    DWORD serverPid{};
    std::string buffer;
    int next = 0;
    unsigned consentCount = 0;
    bool consentHandled = false;
    bool acceptConsent = true;
    bool respondConsent = true;
    bool stopOnConsent = false;

  public:
    explicit Client(std::filesystem::path executable, std::string language = "", unsigned timeout = 0)
    {
        // Start the test MCP client; executable/language select its server and timeout optionally overrides seconds.
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE childInput{}, parentInput{}, parentOutput{}, childOutput{};
        ensure(CreatePipe(&childInput, &parentInput, &sa, 65536), "stdin pipe");
        H ci(childInput);
        input.h = parentInput;
        ensure(CreatePipe(&parentOutput, &childOutput, &sa, 65536), "stdout pipe");
        H co(childOutput);
        output.h = parentOutput;
        SetHandleInformation(input.h, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output.h, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = ci.h;
        si.hStdOutput = co.h;
        si.hStdError = co.h;
        PROCESS_INFORMATION pi{};
        auto cmd = L"\"" + executable.wstring() + L"\"";
        if (language == "ru")
            cmd += L" --language ru";
        if (language == "en")
            cmd += L" --language en";
        if (timeout)
            cmd += L" --timeout " + std::to_wstring(timeout);
        ensure(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                              &si, &pi),
               "server launch");
        process.h = pi.hProcess;
        serverPid = pi.dwProcessId;
        // A local MCP host can explicitly pass its foreground permission to its child.
        record("foreground_permission", {{"granted", bool(AllowSetForegroundWindow(pi.dwProcessId))}});
        H thread(pi.hThread);
    }
    ~Client()
    {
        if (input.h)
        {
            CloseHandle(input.h);
            input.h = nullptr;
        }
        if (process.h && WaitForSingleObject(process.h, 7000) != WAIT_OBJECT_0)
            TerminateProcess(process.h, 9);
    }
    int send(std::string method, json params)
    {
        int id = ++next;
        auto line =
            json({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}).dump() + "\n";
        DWORD written{};
        ensure(WriteFile(input.h, line.data(), (DWORD)line.size(), &written, nullptr) &&
                   written == line.size(),
               "request write");
        return id;
    }
    void set_consent_response(bool allow)
    {
        // Choose the test fixture's consent response; allow=true accepts, false declines.
        acceptConsent = allow;
        respondConsent = true;
    }
    void ignore_consent()
    {
        // Let only this test server's dialog expire without a synthetic response.
        respondConsent = false;
    }
    void stop_on_consent()
    {
        // Cancel the test request through MCP Stop when its own dialog opens.
        respondConsent = false;
        stopOnConsent = true;
    }
    unsigned consent_count() const
    {
        // Return how many session dialogs this test client observed; takes no parameters.
        return consentCount;
    }
    json read(unsigned timeout = 25000)
    {
        // Read an MCP response; synthetic responses target only this test server's consent window.
        auto start = Clock::now();
        while (Clock::now() - start < std::chrono::milliseconds(timeout))
        {
            auto end = buffer.find('\n');
            if (end != std::string::npos)
            {
                auto line = buffer.substr(0, end);
                buffer.erase(0, end + 1);
                return json::parse(line);
            }
            HWND consent = FindWindowW(L"DesktopControlSessionConsent", nullptr);
            if (!consent)
                consentHandled = false;
            else if (!consentHandled && ULONG_PTR(GetPropW(consent, L"DesktopControlServerPid")) == serverPid)
            {
                consentHandled = true;
                ++consentCount;
                if (respondConsent)
                    SendMessageW(GetDlgItem(consent, acceptConsent ? 1 : 2), BM_CLICK, 0, 0);
                if (stopOnConsent)
                {
                    stopOnConsent = false;
                    send("tools/call", {{"name", "stop"}, {"arguments", json::object()}});
                }
                record("session_consent_test", {{"count", consentCount},
                                               {"source", respondConsent ? "synthetic_fixture_confirmation" : "timeout_fixture"}});
            }
            DWORD available{};
            ensure(PeekNamedPipe(output.h, nullptr, 0, nullptr, &available, nullptr), "response pipe closed");
            if (available)
            {
                char bytes[32768];
                DWORD count{};
                ensure(ReadFile(output.h, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr),
                       "response read");
                buffer.append(bytes, count);
            }
            else
                Sleep(5);
        }
        throw std::runtime_error("MCP response timeout");
    }
    json call(std::string const &method, json params)
    {
        auto id = send(method, params);
        auto response = read();
        ensure(response.at("id") == id, "response id");
        ensure(!response.contains("error"), response.dump());
        return response.at("result");
    }
    json raw_tool(std::string const &name, json args = json::object())
    {
        return call("tools/call", {{"name", name}, {"arguments", args}});
    }
    json tool(std::string const &name, json args = json::object())
    {
        auto response = raw_tool(name, args);
        ensure(!response.value("isError", false), name + ": " + response.dump());
        return response;
    }
    void local_stop_button()
    {
        EnumWindows(
            [](HWND hwnd, LPARAM context) -> BOOL {
                DWORD pid{};
                GetWindowThreadProcessId(hwnd, &pid);
                auto expected = DWORD(context);
                wchar_t name[128]{};
                GetClassNameW(hwnd, name, 128);
                RECT r{};
                GetClientRect(hwnd, &r);
                if (pid == expected && wcscmp(name, L"DesktopControlOverlay") == 0 && r.right > 200 &&
                    r.bottom > 80)
                    PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(r.right - 50, r.bottom - 20));
                return TRUE;
            },
            LPARAM(serverPid));
    }
    void destroy_indicator_strip()
    {
        EnumWindows(
            [](HWND hwnd, LPARAM context) -> BOOL {
                DWORD pid{};
                GetWindowThreadProcessId(hwnd, &pid);
                wchar_t cls[128]{};
                GetClassNameW(hwnd, cls, 128);
                RECT r{};
                GetClientRect(hwnd, &r);
                if (pid == DWORD(context) && wcscmp(cls, L"DesktopControlOverlay") == 0 && r.bottom <= 8)
                {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return FALSE;
                }
                return TRUE;
            },
            LPARAM(serverPid));
    }
};
struct FixtureCleanup
{
    DWORD pid{};
    ~FixtureCleanup()
    {
        if (!pid)
            return;
        EnumWindows(
            [](HWND h, LPARAM context) -> BOOL {
                DWORD p{};
                GetWindowThreadProcessId(h, &p);
                wchar_t cls[128]{};
                GetClassNameW(h, cls, 128);
                if (p == DWORD(context) && wcscmp(cls, L"DesktopControlFixture") == 0)
                    PostMessageW(h, WM_CLOSE, 0, 0);
                return TRUE;
            },
            LPARAM(pid));
    }
};
HWND fixture_handle(DWORD expected)
{
    struct Context
    {
        DWORD pid;
        HWND result{};
    } context{expected};
    EnumWindows(
        [](HWND h, LPARAM raw) -> BOOL {
            auto &c = *reinterpret_cast<Context *>(raw);
            DWORD pid{};
            GetWindowThreadProcessId(h, &pid);
            wchar_t name[128]{};
            GetClassNameW(h, name, 128);
            if (pid == c.pid && wcscmp(name, L"DesktopControlFixture") == 0)
            {
                c.result = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
    ensure(context.result != nullptr, "own fixture handle");
    return context.result;
}
std::vector<unsigned char> decode(std::string const &text)
{
    std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> out;
    unsigned value = 0;
    int bits = -8;
    for (char c : text)
    {
        if (c == '=')
            break;
        auto index = chars.find(c);
        ensure(index != std::string::npos, "base64");
        value = (value << 6) | unsigned(index);
        bits += 6;
        if (bits >= 0)
        {
            out.push_back(static_cast<unsigned char>((value >> bits) & 255));
            bits -= 8;
        }
    }
    return out;
}
void save_image(json const &response, std::filesystem::path path)
{
    for (auto const &item : response.at("content"))
        if (item["type"] == "image")
        {
            auto bytes = decode(item.at("data"));
            ensure(bytes.size() > 8 && bytes[0] == 137 && bytes[1] == 80 && bytes[2] == 78 && bytes[3] == 71,
                   "valid PNG");
            std::ofstream f(path, std::ios::binary);
            f.write(reinterpret_cast<char *>(bytes.data()), bytes.size());
            return;
        }
    throw std::runtime_error("Missing image");
}
json observe(Client &c, std::string const &window, bool text = true, bool warp = false)
{
    return c.tool("observe", {{"window_id", window},
                              {"include_screenshot", true},
                              {"include_text", text},
                              {"backend", warp ? "warp" : "auto"}});
}
json data(json const &response)
{
    return response.at("structuredContent");
}
json find_element(json const &observation, int type, std::string aid = "")
{
    for (auto const &e : observation.at("accessibility").at("elements"))
        if (e["control_type"] == type && (aid.empty() || e["automation_id"] == aid))
            return e;
    throw std::runtime_error("Element not found: " + aid);
}
json coords(std::string window, json const &obs, json const &element)
{
    auto b = element.at("bounds"), g = obs.at("geometry");
    return {{"window_id", window},
            {"observation_id", obs.at("observation_id")},
            {"x", b["x"].get<double>() - g["x"].get<double>() + b["width"].get<double>() / 2},
            {"y", b["y"].get<double>() - g["y"].get<double>() + b["height"].get<double>() / 2}};
}
#include "acceptance.hpp"
#include "foreground.hpp"
#include "consent.hpp"
int wmain()
{
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    auto directory = std::filesystem::path(path).parent_path();
    json config = json::object();
    {
        std::ifstream f(directory / L"integration-config.json");
        if (f)
            f >> config;
    }
    auto results = config.contains("results_dir")
                       ? std::filesystem::path(config["results_dir"].get<std::string>())
                       : directory / L"integration-results";
    std::filesystem::create_directories(results);
    logFile.open(results / L"integration.jsonl");
    std::atomic<bool> finished = false;
    std::thread watchdog([&] {
        for (int i = 0; i < 2400 && !finished; ++i)
            Sleep(100);
        if (!finished)
        {
            record("FAIL", {{"message", "integration watchdog"}});
            ExitProcess(10);
        }
    });
    int result = 1;
    FixtureCleanup cleanup;
    try
    {
        Client c(directory / L"desktop-control.exe", config.value("language", ""), config.value("timeout_seconds", unsigned(0)));
        c.call("initialize", {{"protocolVersion", "2025-11-25"},
                              {"capabilities", json::object()},
                              {"clientInfo", {{"name", "native-integration-test"}, {"version", "1"}}}});
        if (config.contains("timeout_seconds"))
        {
            auto effective = data(c.tool("diagnostics"))["timeouts"];
            ensure(effective["override_ms"] == config["timeout_seconds"].get<unsigned>() * 1000,
                   "CLI timeout reached the stdio MCP session");
            record("cli_timeout_applied", effective);
        }
        auto task = data(c.tool("begin_task", {{"title", "Desktop Control integration verification"}}));
        ensure(task["indicator_ready"].get<bool>(), "visible indicator ready");
        ensure(task["overlays"]["blue_strip_count"].get<int>() >= 4, "screen border strips");
        ensure(task["overlays"]["capture_excluded"].get<bool>(), "capture excluded indicators");
        record("indicator_ready", task);
        if (config.value("scenario", "fixture") == "indicator_failure")
        {
            c.destroy_indicator_strip();
            Sleep(200);
            auto state = data(c.tool("diagnostics"))["state"];
            ensure(state["paused"].get<bool>() && state["indicator_failed"].get<bool>() &&
                       !state["indicator_ready"].get<bool>(),
                   "indicator failure pauses automation");
            ensure(c.raw_tool("launch_app", {{"app_id", "C:\\nonexistent-test.exe"}})["isError"].get<bool>(),
                   "automation refused without indicator");
            ensure(c.raw_tool("resume")["isError"].get<bool>(), "resume refused without indicator");
            record("indicator_failure_stops_automation", state);
        }
        else if (config.value("scenario", "fixture") == "foreground")
        {
            foreground_test(c, directory, results);
        }
        else if (config.value("scenario", "fixture") == "consent")
        {
            c.tool("end_task");
            consent_test(directory);
        }
        else if (config.value("scenario", "fixture") == "acceptance")
        {
            acceptance(c, results, config.value("paint_only", false));
        }
        else if (config.value("scenario", "fixture") == "uwp_probe")
        {
            uwp_probe(c, results);
        }
        else
        {
            auto launch =
                data(c.tool("launch_app",
                            {{"app_id", utf8((directory / std::filesystem::path(config.value(
                                                              "fixture_name", "desktop-control-fixture.exe")))
                                                 .wstring())}}));
            ensure(launch["windows"].size() == 1, "one fixture window");
            cleanup.pid = launch["pid"];
            auto window = launch["windows"][0]["window_id"].get<std::string>();
            auto first = observe(c, window);
            save_image(first, results / L"fixture-before.png");
            auto obs = data(first);
            ensure(obs["coordinate_mapping_valid"].get<bool>(), "verified geometry mapping");
            record("capture_and_uia", {{"geometry", obs["geometry"]},
                                       {"image", obs["image"]},
                                       {"elements", obs["accessibility"]["elements"].size()}});
            auto edit = find_element(obs, 50004);
            std::string value = "Привет, Windows! Unicode: 😀";
            auto set = data(c.tool("element_action", {{"window_id", window},
                                                      {"observation_id", obs["observation_id"]},
                                                      {"element_id", edit["element_id"]},
                                                      {"action", "set_value"},
                                                      {"value", value}}));
            ensure(set.contains("observation"), "post-action observation");
            obs = set["observation"];
            ensure(find_element(obs, 50004).value("value", "") == value, "UIA Unicode value");
            record("uia_unicode_set_value");
            edit = find_element(obs, 50004);
            auto click = coords(window, obs, edit);
            auto clicked = data(c.tool("click", click));
            ensure(clicked.contains("observation"), "click observation");
            auto stale = c.raw_tool("click", click);
            ensure(stale["isError"].get<bool>() && data(stale)["error"]["code"] == "STALE_OBSERVATION",
                   "stale click rejected");
            record("stale_observation_rejected");
            c.tool("press_key", {{"window_id", window}, {"key", "Ctrl+A"}});
            auto typed = data(c.tool("type_text", {{"window_id", window}, {"text", value}}));
            ensure(typed.contains("observation"), "typing observation");
            obs = typed["observation"];
            record("unicode_input_actual",
                   {{"actual", find_element(obs, 50004).value("value", "")}, {"expected", value}});
            ensure(find_element(obs, 50004).value("value", "") == value, "SendInput Unicode value");
            record("unicode_input");
            auto report =
                data(c.tool("report_agent_status", {{"task_id", task["task_id"]}, {"phase", "thinking"}}));
            auto diagnostic = data(c.tool("diagnostics"));
            ensure(diagnostic["state"]["phase"] == "thinking", "explicit thinking status");
            record("explicit_thinking");
            if (config.value("status_expiry", false))
            {
                Sleep(31000);
                ensure(data(c.tool("diagnostics"))["state"]["phase"] == "waiting_ai",
                       "client status expires");
                record("client_status_expired");
            }
            first = observe(c, window);
            obs = data(first);
            diagnostic = data(c.tool("diagnostics"));
            ensure(diagnostic["state"]["phase"] == "waiting_ai", "no invented model status");
            c.tool("report_agent_status", {{"task_id", task["task_id"]},
                                           {"phase", "processing_screenshot"},
                                           {"observation_id", obs["observation_id"]}});
            ensure(data(c.tool("diagnostics"))["state"]["phase"] == "processing_screenshot",
                   "explicit screenshot status");
            record("explicit_screenshot_analysis");
            auto button = find_element(obs, 50000);
            auto invoked = data(c.tool("element_action", {{"window_id", window},
                                                          {"observation_id", obs["observation_id"]},
                                                          {"element_id", button["element_id"]},
                                                          {"action", "invoke"}}));
            obs = invoked.at("observation");
            ensure(find_element(obs, 50004).value("value", "") == "Button invoked", "invoke changed edit");
            record("uia_invoke");
            if (config.value("geometry_test", false))
            {
                auto handle = fixture_handle(cleanup.pid);
                auto prior = coords(window, obs, find_element(obs, 50004));
                ensure(SetWindowPos(handle, nullptr, 300, 200, 680, 570, SWP_NOZORDER | SWP_NOACTIVATE),
                       "resize fixture");
                Sleep(200);
                auto rejected = c.raw_tool("click", prior);
                ensure(rejected["isError"].get<bool>() &&
                           data(rejected)["error"]["code"] == "GEOMETRY_CHANGED",
                       "old geometry rejected");
                first = observe(c, window);
                obs = data(first);
                ensure(obs["coordinate_mapping_valid"].get<bool>(), "resized image mapping");
                save_image(first, results / L"fixture-resized.png");
                record("move_resize_geometry",
                       {{"geometry", obs["geometry"]}, {"transform", obs["image_to_screen"]}});
                ShowWindow(handle, SW_MINIMIZE);
                Sleep(200);
                auto minimized = c.raw_tool("observe", {{"window_id", window}});
                ensure(minimized["isError"].get<bool>() &&
                           data(minimized)["error"]["code"] == "WINDOW_UNAVAILABLE",
                       "minimized capture rejected");
                ensure(data(c.tool("diagnostics"))["state"]["overlays"]["yellow_strip_count"].get<int>() == 0,
                       "minimized yellow frame hidden");
                ShowWindow(handle, SW_RESTORE);
                Sleep(200);
                obs = data(observe(c, window));
                record("minimize_restore");
            }
            int cycles = config.value("cycles", 100);
            for (bool warp : {false, true})
            {
                auto start = Clock::now();
                for (int i = 0; i < cycles; ++i)
                {
                    auto response = observe(c, window, false, warp);
                    if (i == cycles - 1)
                        save_image(response,
                                   results / (warp ? L"fixture-warp.png" : L"fixture-hardware.png"));
                    if (i % 20 == 0)
                        record("capture_progress",
                               {{"backend", warp ? "warp" : "hardware"}, {"count", i + 1}});
                }
                record("capture_series",
                       {{"backend", warp ? "warp" : "hardware"},
                        {"count", cycles},
                        {"elapsed_ms",
                         std::chrono::duration<double, std::milli>(Clock::now() - start).count()}});
            }
            obs = data(observe(c, window));
            json points = json::array({{{"x", 70}, {"y", 280}}, {{"x", 400}, {"y", 380}}});
            int dragId = c.send("tools/call", {{"name", "drag"},
                                               {"arguments",
                                                {{"window_id", window},
                                                 {"observation_id", obs["observation_id"]},
                                                 {"points", points},
                                                 {"duration_ms", 3000}}}});
            Sleep(200);
            int stopId = c.send("tools/call", {{"name", "stop"}, {"arguments", json::object()}});
            auto dragResult = c.read(), stopResult = c.read();
            ensure(dragResult["id"] == dragId && dragResult["result"]["isError"].get<bool>(),
                   "drag cancelled");
            ensure(stopResult["id"] == stopId &&
                       stopResult["result"]["structuredContent"]["paused"].get<bool>(),
                   "stop paused");
            ensure(!(GetAsyncKeyState(VK_LBUTTON) & 0x8000), "mouse released");
            record("stop_during_drag", dragResult["result"]["structuredContent"]);
            c.tool("resume");
            c.local_stop_button();
            Sleep(200);
            ensure(data(c.tool("diagnostics"))["state"]["paused"].get<bool>(), "local Stop button pauses");
            record("local_stop_button");
            c.tool("resume");
            c.tool("press_key", {{"window_id", window}, {"key", "Escape"}});
            ensure(!data(c.tool("diagnostics"))["state"]["paused"].get<bool>(),
                   "synthetic Escape is not physical stop");
            record("synthetic_escape_does_not_stop");
            c.tool("close_window", {{"window_id", window}});
        }
        c.tool("end_task");
        record("PASS");
        result = 0;
    }
    catch (std::exception const &e)
    {
        record("FAIL", {{"message", e.what()}});
    }
    finished = true;
    watchdog.join();
    return result;
}
