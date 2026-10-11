#pragma once
// Included after the native MCP client helpers in integration.cpp.
void save_metadata(json value, std::filesystem::path path)
{
    if (value.contains("image"))
        value["image"].erase("data");
    std::ofstream(path) << value.dump(2);
}
std::string launched_window(Client &c, json args)
{
    auto result = data(c.tool("launch_app", args));
    ensure(!result["windows"].empty(), "launched application window");
    record("application_launched", result);
    Sleep(1200); // Let the app complete its own initial foreground activation.
    return result["windows"][0]["window_id"];
}
void acceptance(Client &c, std::filesystem::path const &results, bool paintOnly)
{
    wchar_t system[32768]{};
    GetSystemDirectoryW(system, 32768);
    if (!paintOnly)
    {
        auto notepad = launched_window(
            c, {{"app_id", utf8((std::filesystem::path(system) / L"notepad.exe").wstring())}});
        auto n = data(observe(c, notepad));
        auto edit = find_element(n, 50004);
        c.tool("click", coords(notepad, n, edit));
        std::string value = "Desktop Control: Привет, Windows! 😀";
        c.tool("type_text", {{"window_id", notepad}, {"text", value}});
        auto nr = observe(c, notepad);
        n = data(nr);
        save_image(nr, results / L"notepad.png");
        save_metadata(n, results / L"notepad.json");
        edit = find_element(n, 50004);
        ensure(edit.value("value", edit.value("text", "")).find(value) != std::string::npos,
               "Notepad Unicode readback");
        record("notepad_unicode_readback");
        c.tool("press_key", {{"window_id", notepad}, {"key", "Ctrl+A"}});
        c.tool("press_key", {{"window_id", notepad}, {"key", "Backspace"}});
        c.tool("close_window", {{"window_id", notepad}});

        auto apps = data(c.tool("list_apps"))["apps"];
        std::string aumid;
        for (auto const &app : apps)
            if (app.value("app_id", "").find("Microsoft.WindowsCalculator_") == 0)
                aumid = app["app_id"];
        if (aumid.empty())
        {
            record("SKIP_calculator",
                   {{"reason", "Windows Calculator is not installed in this user profile"}});
        }
        else
        {

            for (auto const &w : data(c.tool("list_windows"))["windows"])
                ensure(w.value("aumid", "") != aumid,
                       "Close existing Calculator before running scratch acceptance");
            auto calc = launched_window(c, {{"app_id", aumid}});
            auto cr = observe(c, calc);
            auto co = data(cr);
            save_metadata(co, results / L"calculator-initial.json");
            auto invoke = [&](std::vector<std::string> ids) {
                json element;
                for (auto const &e : co["accessibility"]["elements"])
                    for (auto const &id : ids)
                        if (e.value("automation_id", "") == id)
                            element = e;
                ensure(!element.is_null(), "Calculator button: " + ids.front());
                auto r = data(c.tool("element_action", {{"window_id", calc},
                                                        {"observation_id", co["observation_id"]},
                                                        {"element_id", element["element_id"]},
                                                        {"action", "invoke"}}));
                co = r.at("observation");
            };
            invoke({"clearButton"});
            invoke({"num2Button"});
            invoke({"plusButton", "addButton"});
            invoke({"num3Button"});
            invoke({"equalButton"});
            cr = observe(c, calc);
            co = data(cr);
            save_image(cr, results / L"calculator.png");
            save_metadata(co, results / L"calculator.json");
            bool five = false;
            for (auto const &e : co["accessibility"]["elements"])
                if (e.value("automation_id", "") == "CalculatorResults")
                    five = e.value("name", "").find('5') != std::string::npos;
            ensure(five, "Calculator 2+3=5 readback");
            record("calculator_2_plus_3");
            c.tool("close_window", {{"window_id", calc}});
        }
    }
    // Own image file, never an existing user document.
    auto bmp = results / L"paint-house.bmp";
    const int width = 800, height = 500, stride = width * 3;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + stride * height;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biSizeImage = stride * height;
    {
        std::ofstream f(bmp, std::ios::binary);
        f.write(reinterpret_cast<char *>(&file), sizeof(file));
        f.write(reinterpret_cast<char *>(&info), sizeof(info));
        std::vector<char> white(stride * height, char(255));
        f.write(white.data(), white.size());
    }
    auto paint =
        launched_window(c, {{"app_id", utf8((std::filesystem::path(system) / L"mspaint.exe").wstring())},
                            {"arguments", json::array({utf8(bmp.wstring())})}});
    auto pr = observe(c, paint);
    auto po = data(pr);
    save_image(pr, results / L"paint-initial.png");
    save_metadata(po, results / L"paint-initial.json");
    record("paint_waiting_for_verified_coordinates", {{"window_id", paint}, {"geometry", po["geometry"]}});
    auto actionsPath = results / L"paint-actions.json";
    auto start = Clock::now();
    while (!std::filesystem::exists(actionsPath) && Clock::now() - start < std::chrono::seconds(120))
        Sleep(100);
    ensure(std::filesystem::exists(actionsPath), "Paint coordinate instructions timeout");
    json actions;
    {
        std::ifstream f(actionsPath);
        f >> actions;
    }
    for (auto const &stroke : actions.at("strokes"))
    {
        po = data(observe(c, paint));
        c.tool("drag", {{"window_id", paint},
                        {"observation_id", po["observation_id"]},
                        {"points", stroke},
                        {"duration_ms", 500}});
    }
    pr = observe(c, paint);
    save_image(pr, results / L"paint-house.png");
    save_metadata(data(pr), results / L"paint-house.json");
    c.tool("press_key", {{"window_id", paint}, {"key", "Ctrl+S"}});
    c.tool("close_window", {{"window_id", paint}});
    record("paint_house_drawn");
}
void uwp_probe(Client &c, std::filesystem::path const &results)
{
    auto apps = data(c.tool("list_apps"))["apps"];
    std::string aumid;
    json packaged = json::array();
    for (auto const &app : apps)
    {
        if (app.value("kind", "") == "packaged")
            packaged.push_back(app);
        if (app.value("app_id", "").find("64360VelerSoftware.DevToys_") == 0)
            aumid = app["app_id"];
    }
    std::ofstream(results / L"packaged-apps.json") << packaged.dump(2);
    ensure(!aumid.empty(), "DevToys UWP catalog entry");
    for (auto const &w : data(c.tool("list_windows"))["windows"])
        ensure(w.value("aumid", "") != aumid, "DevToys already open: scratch test would reuse user window");
    auto window = launched_window(c, {{"app_id", aumid}});
    auto response = observe(c, window);
    auto obs = data(response);
    save_image(response, results / L"uwp.png");
    save_metadata(obs, results / L"uwp.json");
    ensure(obs["accessibility"]["elements"].size() > 1, "UWP accessibility tree");
    auto search = find_element(obs, 50004, "TextBox");
    std::string value = "Привет, UWP! 😀";
    auto updated = data(c.tool("element_action", {{"window_id", window},
                                                  {"observation_id", obs["observation_id"]},
                                                  {"element_id", search["element_id"]},
                                                  {"action", "set_value"},
                                                  {"value", value}}));
    obs = updated.at("observation");
    search = find_element(obs, 50004, "TextBox");
    ensure(search.value("value", "") == value, "UWP Unicode value readback");
    response = observe(c, window);
    save_image(response, results / L"uwp-unicode.png");
    obs = data(response);
    save_metadata(obs, results / L"uwp-unicode.json");
    search = find_element(obs, 50004, "TextBox");
    c.tool("element_action", {{"window_id", window},
                              {"observation_id", obs["observation_id"]},
                              {"element_id", search["element_id"]},
                              {"action", "set_value"},
                              {"value", ""}});
    record("uwp_unicode_set_value_readback");
    c.tool("close_window", {{"window_id", window}});
    record("uwp_launch_capture_uia");
}
