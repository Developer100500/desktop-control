#include "control.hpp"
#include <algorithm>
#include <cstdio>
namespace dc
{
namespace
{
json property(std::string type, std::string description = "")
{
    json out = {{"type", type}};
    if (!description.empty())
        out["description"] = description;
    return out;
}
json string_enum(std::initializer_list<const char *> values)
{
    auto p = property("string");
    p["enum"] = values;
    return p;
}
json error_content(Error const &e, double elapsed)
{
    json data = {{"outcome", e.unknown ? "unknown" : "rejected"},
                 {"error",
                  {{"code", e.code},
                   {"message", e.what()},
                   {"stage", e.stage},
                   {"hresult", e.hresult},
                   {"elapsed_ms", elapsed}}}};
    if (e.stage == "focus")
        data["foreground_permission"] = foreground_information();
    return {{"isError", true},
            {"structuredContent", data},
            {"content", json::array({{{"type", "text"}, {"text", data.dump()}}})}};
}
void validate(json const &schema, json const &value, std::string path = "arguments")
{
    if (schema.contains("type"))
    {
        auto t = schema["type"].get<std::string>();
        bool ok = (t == "object" && value.is_object()) || (t == "array" && value.is_array()) ||
                  (t == "string" && value.is_string()) || (t == "boolean" && value.is_boolean()) ||
                  (t == "integer" && value.is_number_integer()) || (t == "number" && value.is_number());
        if (!ok)
            throw Error("INVALID_ARGUMENT", path + " has the wrong type");
    }
    if (schema.contains("enum") &&
        std::find(schema["enum"].begin(), schema["enum"].end(), value) == schema["enum"].end())
        throw Error("INVALID_ARGUMENT", path + " has an unsupported value");
    if (value.is_object())
    {
        for (auto const &key : schema.value("required", json::array()))
            if (!value.contains(key.get<std::string>()))
                throw Error("INVALID_ARGUMENT", path + " is missing " + key.get<std::string>());
        for (auto it = value.begin(); it != value.end(); ++it)
        {
            if (!schema["properties"].contains(it.key()))
                throw Error("INVALID_ARGUMENT", path + " contains unknown property " + it.key());
            validate(schema["properties"][it.key()], it.value(), path + "." + it.key());
        }
    }
    if (value.is_array() && schema.contains("items"))
        for (auto const &item : value)
            validate(schema["items"], item, path + "[]");
}
} // namespace
Server::Server()
    : indicator_(state_), job_(CreateJobObjectW(nullptr, nullptr)), desktop_(state_, job_.value),
      capture_(state_, "capture", job_.value), uia_(state_, "uia", job_.value),
      launch_(state_, "launch", job_.value)
{
    check(bool(job_), "JOB_CREATE", "Cannot create worker job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    check(SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
          "JOB_CREATE", "Cannot set worker job limits");
}
Server::~Server()
{
    state_.stop();
    desktop_.release_input();
    capture_.reset();
    uia_.reset();
    launch_.reset();
    state_.quitting = true;
}
void Server::write(json const &message)
{
    std::lock_guard lock(output_mutex_);
    std::cout << message.dump() << '\n' << std::flush;
}
json Server::tools()
{
    // Describe all supported MCP tools and their arguments; takes no parameters.
    json list = json::array();
    auto add = [&](std::string name, std::string description, json props, json required,
                   bool readonly = false, bool destructive = true) {
        list.push_back({{"name", name},
                        {"description", description},
                        {"inputSchema",
                         {{"type", "object"},
                          {"properties", props},
                          {"required", required},
                          {"additionalProperties", false}}},
                        {"annotations",
                         {{"readOnlyHint", readonly},
                          {"destructiveHint", !readonly && destructive},
                          {"idempotentHint", readonly},
                          {"openWorldHint", false}}}});
    };
    json empty = json::object();
    auto s = property("string"), b = property("boolean"), n = property("number"), i = property("integer");
    json window = {{"window_id", s}};
    json observed = window;
    observed["observation_id"] = s;
    json coordinate = observed;
    coordinate["x"] = n;
    coordinate["y"] = n;
    add("begin_task", "Request permission for this connection and show indicators before capture or automation. After an unsuccessful request, retry only when the user explicitly asks, using retry_consent=true.",
        {{"title", s},
         {"retry_consent", property("boolean", "Default false. Reopen the permission dialog after a failed request only on an explicit user request; never grants permission automatically.")}},
        json::array({"title"}), false, false);
    add("end_task", "Finish the task and remove control indicators.", empty, json::array(), false, false);
    add("list_apps", "List installed packaged apps and discoverable Win32 apps.", empty, json::array(), true);
    add("list_windows", "List visible Win32 and UWP windows. Choose exactly one window_id.", empty,
        json::array(), true);
    add("launch_app", "Launch an absolute .exe path or packaged AUMID; requires begin_task.",
        {{"app_id", s}, {"arguments", {{"type", "array"}, {"items", s}}}}, json::array({"app_id"}));
    add("close_window", "Request normal close; save dialogs are not dismissed.", window,
        json::array({"window_id"}));
    auto activate = window;
    activate["foreground_prompt"] = property("boolean", "Deprecated compatibility argument; never opens a per-window permission dialog.");
    activate["cursor_fallback"] = property("boolean", "Default true: if Windows denies normal activation, click a verified title-bar point.");
    activate["observation_id"] = s;
    activate["x"] = n;
    activate["y"] = n;
    add("activate_window", "Activate normally, then use the cursor on the title bar if needed. For custom windows with no safe title bar, provide x/y and observation_id from a fresh screenshot. No additional permission dialogs.", activate,
        json::array({"window_id"}));
    auto observeProps = window;
    observeProps["include_screenshot"] = b;
    observeProps["include_text"] = b;
    observeProps["backend"] = string_enum({"auto", "warp"});
    add("observe",
        "Observe a selected window. Screenshot defaults to true; UIA to false. Returns fresh observation_id.",
        observeProps, json::array({"window_id"}), true);
    auto action = observed;
    action["element_id"] = s;
    action["action"] =
        string_enum({"invoke", "set_value", "toggle", "select", "expand", "collapse", "scroll"});
    action["value"] = s;
    action["horizontal"] = i;
    action["vertical"] = i;
    add("element_action",
        "Use a UIA pattern on an element from the latest observation. Always observe after an action.",
        action, json::array({"window_id", "observation_id", "element_id", "action"}));
    auto click = coordinate;
    click["button"] = string_enum({"left", "right", "middle"});
    click["count"] = i;
    add("click", "Click screenshot pixel coordinates; requires a valid observation and unobstructed target.",
        click, json::array({"window_id", "observation_id", "x", "y"}));
    auto scroll = coordinate;
    scroll["vertical"] = i;
    scroll["horizontal"] = i;
    add("scroll", "Scroll at image coordinates. Wheel deltas are Windows units: 120 per notch.", scroll,
        json::array({"window_id", "observation_id", "x", "y"}));
    auto drag = observed;
    drag["points"] = {{"type", "array"},
                      {"items",
                       {{"type", "object"},
                        {"properties", {{"x", n}, {"y", n}}},
                        {"required", json::array({"x", "y"})},
                        {"additionalProperties", false}}}};
    drag["duration_ms"] = i;
    add("drag", "Drag through 2..1000 image points, for at most 5000 ms.", drag,
        json::array({"window_id", "observation_id", "points"}));
    auto keys = window;
    keys["key"] = s;
    keys["keys"] = {{"type", "array"}, {"items", s}};
    add("press_key", "Send a chord such as Ctrl+S, or an array of key names. Target must have focus.", keys,
        json::array({"window_id"}));
    auto text = window;
    text["text"] = s;
    add("type_text", "Send literal Unicode text to the focused target window.", text,
        json::array({"window_id", "text"}));
    add("report_agent_status",
        "Explicitly report thinking or screenshot analysis, valid for 30 seconds. Never inferred from "
        "inactivity.",
        {{"task_id", s},
         {"phase", string_enum({"thinking", "processing_screenshot"})},
         {"observation_id", s}},
        json::array({"task_id", "phase"}), false, false);
    add("stop", "Immediately pause and cancel automation.", empty, json::array(), false, false);
    add("resume", "Explicitly resume a paused task after checking its indicator.", empty, json::array(), false, false);
    add("diagnostics", "Return engine, session and task state without screenshots or typed text.", empty,
        json::array(), true);
    return list;
}
json Server::observe(json const &args)
{
    state_.require_active();
    auto w = desktop_.resolve(args.at("window_id"));
    state_.set_target(w);
    bool screenshot = args.value("include_screenshot", true), text = args.value("include_text", false);
    if (!screenshot && !text)
        throw Error("EMPTY_OBSERVATION", "Request a screenshot, UIA, or both");
    if (!IsWindowVisible(w.hwnd) || IsIconic(w.hwnd))
        throw Error("WINDOW_UNAVAILABLE", "Target is hidden or minimized");
    auto geometry = window_rect(w.hwnd);
    auto id = uuid();
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char observedAt[40]{};
    std::snprintf(observedAt, sizeof(observedAt), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear,
                  utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
    json result = {
        {"window_id", w.id},
        {"observation_id", id},
        {"observed_at_utc", observedAt},
        {"geometry", rect_json(geometry)},
        {"dpi", GetDpiForWindow(w.hwnd)},
        {"coordinate_mapping_valid", false},
        {"image_to_screen", nullptr},
        {"steady_ms",
         std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()}};
    if (screenshot)
    {
        auto image = capture_.call({{"operation", "observe"},
                                    {"hwnd", reinterpret_cast<uint64_t>(w.hwnd)},
                                    {"warp", args.value("backend", "auto") == "warp"}});
        auto after = window_rect(w.hwnd);
        if (rect_json(after) != rect_json(geometry))
            throw Error("GEOMETRY_CHANGED", "Window moved during capture; observe again");
        result["coordinate_mapping_valid"] = image["width"] == geometry.right - geometry.left &&
                                             image["height"] == geometry.bottom - geometry.top;
        if (result["coordinate_mapping_valid"].get<bool>())
            result["image_to_screen"] = {{"scale_x", 1},
                                         {"scale_y", 1},
                                         {"offset_x", geometry.left},
                                         {"offset_y", geometry.top},
                                         {"units", "physical_pixels"}};
        result["image"] = image;
    }
    if (text)
        result["accessibility"] = uia_.call(
            {{"operation", "observe"}, {"hwnd", reinterpret_cast<uint64_t>(w.hwnd)}, {"observation_id", id}});
    {
        std::lock_guard lock(state_.mutex);
        result["blocked_regions"] = json::array();
        if (state_.overlays.contains("panel"))
            result["blocked_regions"].push_back(state_.overlays["panel"]);
    }
    desktop_.remember(result);
    state_.set_phase("waiting_ai");
    return result;
}
json Server::invoke(std::string const &name, json const &args)
{
    // Dispatch a validated tool call; name selects the tool and args contains its schema-checked inputs.
    if (name == "diagnostics")
        return diagnostic();
    if (name == "list_windows")
        return {{"windows", desktop_.list_windows()}};
    if (name == "list_apps")
        return {{"apps", desktop_.list_apps()}};
    if (name == "begin_task")
    {
        if (!os_version().value("supported", false))
            throw Error("OS_UNSUPPORTED", "Windows 10 22H2 or Windows 11 workstation is required");
        if (!input_desktop_available())
            throw Error("DESKTOP_UNAVAILABLE", "An unlocked interactive desktop is required");
        auto title = args.at("title").get<std::string>();
        if (title.empty() || title.size() > 480 || title.find_first_of("\r\n\t") != std::string::npos)
            throw Error("INVALID_TITLE", "Use a one-line title of 1..480 UTF-8 bytes");
        {
            std::lock_guard lock(state_.mutex);
            if (state_.active)
                throw Error("TASK_ACTIVE", "End the current task first");
            state_.title = title;
            state_.task_id = uuid();
            state_.target = nullptr;
            state_.target_pid = 0;
            state_.target_created = 0;
        }
        request_session_permission(state_, job_.value, args.value("retry_consent", false));
        indicator_.start();
        state_.set_phase("waiting_ai");
        return state_.status();
    }
    if (name == "stop")
    {
        state_.stop();
        desktop_.release_input();
        desktop_.invalidate();
        return state_.status();
    }
    if (name == "resume")
    {
        if (!input_desktop_available())
            throw Error("DESKTOP_UNAVAILABLE", "Interactive desktop is unavailable");
        {
            std::lock_guard lock(state_.mutex);
            if (!state_.active)
                throw Error("TASK_REQUIRED", "No task to resume");
            if (!state_.ready || state_.indicator_failed)
                throw Error("INDICATOR_UNAVAILABLE", "Indicator is unavailable");
            state_.paused = false;
            state_.phase = "waiting_ai";
            state_.client_phase.clear();
        }
        return state_.status();
    }
    if (name == "end_task")
    {
        desktop_.release_input();
        desktop_.invalidate();
        capture_.reset();
        uia_.reset();
        launch_.reset();
        indicator_.finish();
        return state_.status();
    }
    state_.require_active();
    if (name == "report_agent_status")
    {
        auto phase = args.at("phase").get<std::string>();
        if (phase == "processing_screenshot")
        {
            if (!args.contains("observation_id") || !desktop_.image_snapshot(args.at("observation_id")))
                throw Error("OBSERVATION_REQUIRED",
                            "Screenshot analysis needs a current image observation_id");
            std::lock_guard lock(state_.mutex);
            if (state_.task_id != args.at("task_id").get<std::string>())
                throw Error("UNKNOWN_TASK", "Task id does not match");
        }
        else
        {
            std::lock_guard lock(state_.mutex);
            if (state_.task_id != args.at("task_id").get<std::string>())
                throw Error("UNKNOWN_TASK", "Task id does not match");
        }
        std::lock_guard lock(state_.mutex);
        state_.phase = "waiting_ai";
        state_.client_phase = phase;
        state_.client_until = Clock::now() + std::chrono::seconds(30);
        return {{"accepted", true}, {"phase", phase}, {"ttl_seconds", 30}};
    }
    if (name == "observe")
        return observe(args);
    if (name == "launch_app")
    {
        auto result = desktop_.launch(args, launch_);
        desktop_.invalidate();
        state_.set_phase("waiting_ai");
        return result;
    }
    json result;
    if (name == "element_action")
    {
        if (args.at("action") == "set_value" && !args.contains("value"))
            throw Error("INVALID_ARGUMENT", "set_value requires value");
        if (args.at("action") == "scroll")
            for (auto key : {"horizontal", "vertical"})
                if (args.contains(key) && (args[key].get<int>() < 0 || args[key].get<int>() > 4))
                    throw Error("INVALID_ARGUMENT", "UIA ScrollAmount must be 0..4");
        auto obs = desktop_.observation(args);
        if (!obs.contains("accessibility"))
            throw Error("UIA_REQUIRED", "Observe with include_text=true first");
        auto w = desktop_.resolve(args.at("window_id"));
        state_.set_target(w);
        std::string description = localize("action_" + args.at("action").get<std::string>(),
                                           state_.status().value("language", "en") == "ru");
        for (auto const &element : obs["accessibility"]["elements"])
            if (element["element_id"] == args.at("element_id"))
            {
                auto label = wide(element.value("name", ""));
                label.erase(std::remove_if(label.begin(), label.end(), [](wchar_t c) { return c < 32; }),
                            label.end());
                if (label.size() > 80)
                    label.resize(80);
                if (!label.empty() && label.back() >= 0xd800 && label.back() <= 0xdbff)
                    label.pop_back();
                if (!label.empty())
                    description += " — " + utf8(label);
                break;
            }
        state_.set_phase("element_action", description);
        result = uia_.call({{"operation", "action"},
                            {"hwnd", reinterpret_cast<uint64_t>(w.hwnd)},
                            {"observation_id", args.at("observation_id")},
                            {"element_id", args.at("element_id")},
                            {"action", args.at("action")},
                            {"value", args.value("value", "")},
                            {"horizontal", args.value("horizontal", 2)},
                            {"vertical", args.value("vertical", 2)}},
                           5000, true);
        desktop_.invalidate();
    }
    else
        result = desktop_.act(name, args);
    if (name == "close_window")
    {
        result["windows"] = desktop_.list_windows();
        state_.set_phase("waiting_ai");
        return result;
    }
    state_.set_phase("verifying");
    Sleep(100);
    try
    {
        auto fresh = observe({{"window_id", args.at("window_id")},
                              {"include_screenshot", true},
                              {"include_text", name == "element_action" || name == "type_text"}});
        result["observation"] = fresh;
    }
    catch (Error const &e)
    {
        result["outcome"] = "delivered_observation_failed";
        result["observation_error"] = {{"code", e.code}, {"message", e.what()}};
        state_.set_phase("waiting_ai");
    }
    return result;
}
void Server::set_timeout(unsigned seconds)
{
    // Set startup timeout; seconds must be within 1..86400 and is converted to milliseconds.
    if (seconds < 1 || seconds > 86400)
        throw std::invalid_argument("Timeout must be from 1 to 86400 seconds");
    state_.timeout_override_ms = seconds * 1000;
}
json Server::diagnostic()
{
    // Report capabilities and effective wait budgets; takes no parameters.
    DWORD session{};
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return {{"version", "0.1.3"},      {"protocol_version", "2025-11-25"},
            {"architecture", "x64"},   {"os", os_version()},
            {"session_id", session},   {"interactive_desktop", input_desktop_available()},
            {"foreground_permission", foreground_information()},
            {"timeouts", {{"override_ms", state_.timeout_override_ms ? json(state_.timeout_override_ms) : json(nullptr)},
                          {"worker_ms", state_.timeout_ms(5000)},
                          {"observation_ms", state_.timeout_ms(60000)},
                          {"capture_frame_ms", state_.timeout_ms(2000)},
                          {"uia_scan_ms", state_.timeout_ms(3000)},
                          {"launch_ms", state_.timeout_ms(15000)},
                          {"session_consent_ms", state_.timeout_ms(45000)},
                          {"foreground_prompt_ms", state_.timeout_ms(15000)},
                          {"indicator_ms", state_.timeout_ms(5000)},
                          {"title_bar_search_ms", state_.timeout_ms(800)}}},
            {"state", state_.status()}};
}
json Server::worker_recovery_test()
{
    json results = json::array();
    for (auto *worker : {&capture_, &uia_, &launch_})
    {
        auto start = Clock::now();
        bool timeout = false;
        try
        {
            worker->call({{"operation", "test_hang"}, {"hwnd", uint64_t(0)}}, 5000);
        }
        catch (Error const &e)
        {
            timeout = e.code == "WORKER_TIMEOUT";
        }
        if (!timeout)
            throw Error("TEST_FAILED", "Worker watchdog did not fire");
        bool recovered = false;
        try
        {
            worker->call({{"operation", "observe"},
                          {"hwnd", uint64_t(0)},
                          {"observation_id", "test"},
                          {"app_id", "C:\\nonexistent-desktop-control-test.exe"}},
                         5000);
        }
        catch (Error const &e)
        {
            recovered = e.code == "CAPTURE_TARGET" || e.code == "WINDOW_GONE" || e.code == "INVALID_APP";
        }
        if (!recovered)
            throw Error("TEST_FAILED", "Worker did not recover after termination");
        results.push_back(
            {{"timeout", true},
             {"recovered", true},
             {"elapsed_ms", std::chrono::duration<double, std::milli>(Clock::now() - start).count()}});
    }
    return {{"result", "PASS"}, {"workers", results}};
}
void Server::set_language(std::string const &language)
{
    if (language != "en" && language != "ru")
        throw Error("INVALID_LANGUAGE", "Use en or ru");
    std::lock_guard lock(state_.mutex);
    state_.russian = language == "ru";
}
void Server::reader()
{
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.size() > 1024 * 1024)
        {
            write({{"jsonrpc", "2.0"},
                   {"id", nullptr},
                   {"error", {{"code", -32600}, {"message", "Request exceeds 1 MiB"}}}});
            continue;
        }
        json message;
        try
        {
            message = json::parse(line);
        }
        catch (...)
        {
            write({{"jsonrpc", "2.0"},
                   {"id", nullptr},
                   {"error", {{"code", -32700}, {"message", "Invalid JSON"}}}});
            continue;
        }
        if (!message.is_object() || !message.contains("jsonrpc") || !message["jsonrpc"].is_string() ||
            message["jsonrpc"] != "2.0" || !message.contains("method") || !message["method"].is_string())
        {
            write({{"jsonrpc", "2.0"},
                   {"id", message.is_object() ? message.value("id", json(nullptr)) : json(nullptr)},
                   {"error", {{"code", -32600}, {"message", "Invalid JSON-RPC request"}}}});
            continue;
        }
        if (message.contains("params") && !message["params"].is_object())
        {
            write({{"jsonrpc", "2.0"},
                   {"id", message.value("id", json(nullptr))},
                   {"error", {{"code", -32602}, {"message", "params must be an object"}}}});
            continue;
        }
        if (message["method"] == "notifications/cancelled")
        {
            auto id = message.value("params", json::object()).value("requestId", json(nullptr));
            std::lock_guard lock(current_mutex_);
            if (current_request_ == id.dump())
                ++state_.cancellation;
            continue;
        }
        if (message["method"] == "tools/call" &&
            message.value("params", json::object()).value("name", json(nullptr)) == "stop" &&
            message.value("params", json::object()).value("arguments", json::object()).is_object() &&
            message.value("params", json::object()).value("arguments", json::object()).empty())
        {
            state_.stop();
            desktop_
                .release_input(); /* The queued call sends the response after the current action unwinds. */
        }
        {
            std::lock_guard lock(queue_mutex_);
            queue_.push_back(std::move(message));
        }
        queue_ready_.notify_one();
    }
    state_.stop();
    desktop_.release_input();
    {
        std::lock_guard lock(queue_mutex_);
        eof_ = true;
        queue_.clear();
    }
    queue_ready_.notify_all();
}
int Server::run()
{
    std::thread input([this] { reader(); });
    for (;;)
    {
        json request;
        {
            std::unique_lock lock(queue_mutex_);
            queue_ready_.wait(lock, [&] { return !queue_.empty() || eof_; });
            if (queue_.empty() && eof_)
                break;
            request = std::move(queue_.front());
            queue_.pop_front();
        }
        auto method = request["method"].get<std::string>();
        auto id = request.value("id", json(nullptr));
        if (method == "notifications/initialized")
            continue;
        if (!request.contains("id"))
            continue;
        {
            std::lock_guard lock(current_mutex_);
            current_request_ = id.dump();
        }
        auto started = Clock::now();
        try
        {
            json result;
            if (method == "initialize")
            {
                auto requested = request.value("params", json::object()).value("protocolVersion", "");
                result = {{"protocolVersion", "2025-11-25"},
                          {"capabilities", {{"tools", {{"listChanged", false}}}}},
                          {"serverInfo", {{"name", "desktop-control"}, {"version", "0.1.3"}}},
                          {"instructions",
                           "Call begin_task before observation or actions. Refresh observation after "
                           "actions. Use stop to pause. Never infer thinking from inactivity."}};
                initialized_ = true;
                (void)requested;
            }
            else if (method == "ping")
                result = json::object();
            else if (!initialized_)
                throw Error("NOT_INITIALIZED", "Initialize MCP first");
            else if (method == "tools/list")
                result = {{"tools", tools()}};
            else if (method == "tools/call")
            {
                auto params = request.value("params", json::object());
                auto name = params.at("name").get<std::string>();
                auto args = params.value("arguments", json::object());
                auto definitions = tools();
                auto definition = std::find_if(definitions.begin(), definitions.end(),
                                               [&](auto const &d) { return d["name"] == name; });
                if (definition == definitions.end())
                    throw Error("UNKNOWN_TOOL", "Unknown tool: " + name);
                validate((*definition)["inputSchema"], args);
                auto data = invoke(name, args);
                json content = json::array();
                auto addImage = [&](json &object) {
                    if (object.contains("image"))
                    {
                        auto &image = object["image"];
                        content.push_back(
                            {{"type", "image"}, {"mimeType", "image/png"}, {"data", image.at("base64")}});
                        image.erase("base64");
                    }
                };
                addImage(data);
                if (data.contains("observation"))
                    addImage(data["observation"]);
                content.push_back({{"type", "text"}, {"text", data.dump()}});
                result = {{"content", content}, {"structuredContent", data}, {"isError", false}};
            }
            else
            {
                write({{"jsonrpc", "2.0"},
                       {"id", id},
                       {"error", {{"code", -32601}, {"message", "Method not found"}}}});
                continue;
            }
            write({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        }
        catch (Error const &e)
        {
            desktop_.release_input();
            if (e.unknown)
                desktop_.invalidate();
            state_.set_phase("error", e.code);
            auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            if (method == "tools/call")
                write({{"jsonrpc", "2.0"}, {"id", id}, {"result", error_content(e, elapsed)}});
            else
                write({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32600}, {"message", e.what()}}}});
        }
        catch (winrt::hresult_error const &e)
        {
            desktop_.release_input();
            Error err("HRESULT", winrt::to_string(e.message()), "coordinator", e.code().value);
            if (method == "tools/call")
                write(
                    {{"jsonrpc", "2.0"},
                     {"id", id},
                     {"result",
                      error_content(
                          err, std::chrono::duration<double, std::milli>(Clock::now() - started).count())}});
            else
                write(
                    {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32603}, {"message", err.what()}}}});
        }
        catch (std::exception const &e)
        {
            desktop_.release_input();
            if (method == "tools/call")
            {
                Error err("INVALID_ARGUMENT", e.what());
                write(
                    {{"jsonrpc", "2.0"},
                     {"id", id},
                     {"result",
                      error_content(
                          err, std::chrono::duration<double, std::milli>(Clock::now() - started).count())}});
            }
            else
                write({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32602}, {"message", e.what()}}}});
        }
        {
            std::lock_guard lock(current_mutex_);
            current_request_.clear();
        }
    }
    input.join();
    bool visible = false;
    bool active = false;
    {
        std::lock_guard lock(state_.mutex);
        visible = state_.active || state_.finish;
        active = state_.active;
    }
    if (active)
    {
        indicator_.finish();
        std::lock_guard lock(state_.mutex);
        state_.phase = "disconnected";
    }
    if (visible)
        Sleep(2100);
    return 0;
}
} // namespace dc
