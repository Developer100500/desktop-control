#pragma once
#include <windows.h>
#include "json.hpp"
#include <appmodel.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <dwmapi.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <objbase.h>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <winrt/base.h>
namespace dc
{
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Handle
{
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE h) : value(h)
    {
    }
    ~Handle()
    {
        reset();
    }
    Handle(Handle const &) = delete;
    Handle &operator=(Handle const &) = delete;
    Handle(Handle &&h) noexcept : value(std::exchange(h.value, nullptr))
    {
    }
    Handle &operator=(Handle &&h) noexcept
    {
        if (this != &h)
        {
            reset();
            value = std::exchange(h.value, nullptr);
        }
        return *this;
    }
    void reset(HANDLE h = nullptr)
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = h;
    }
    explicit operator bool() const
    {
        return value && value != INVALID_HANDLE_VALUE;
    }
};
struct Error : std::runtime_error
{
    std::string code, stage;
    long hresult{};
    bool unknown{};
    Error(std::string c, std::string message, std::string s = "", long hr = 0, bool u = false)
        : std::runtime_error(message), code(std::move(c)), stage(std::move(s)), hresult(hr), unknown(u)
    {
    }
};
std::wstring wide(std::string const &);
std::string utf8(std::wstring const &);
std::string uuid();
std::string base64(std::vector<uint8_t> const &);
std::wstring executable();
json rect_json(RECT);
RECT window_rect(HWND);
void check(bool, std::string const &, std::string const &);
void worker_event(std::string const &);
void note_window_destroyed(HWND);
json os_version();
bool input_desktop_available();
bool elevated(HWND);
uint64_t process_created(DWORD);
std::string localize(std::string const &, bool russian);
struct Window;
struct State
{
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool active = false, paused = false, ready = false, indicator_failed = false, finish = false;
    bool russian = false;
    unsigned timeout_override_ms = 0;
    unsigned timeout_ms(unsigned fallback) const
    {
        // Resolve a wait budget; fallback preserves the old limit when no CLI override was supplied.
        return timeout_override_ms ? timeout_override_ms : fallback;
    }
    bool session_authorized = false;
    bool session_consent_requested = false;
    HWND target{};
    DWORD target_pid{};
    uint64_t target_created{};
    json overlays = json::object();
    std::string task_id, title, phase = "idle", detail, error;
    Clock::time_point client_until{};
    std::string client_phase;
    std::atomic<bool> quitting{false};
    std::atomic<uint64_t> cancellation{0};
    void set_phase(std::string p, std::string d = "");
    void stop();
    void set_target(Window const &);
    void require_active() const;
    bool cancelled(uint64_t generation) const;
    json status() const;
};
class Indicator
{
    State &state_;
    std::thread thread_;
    void loop();

  public:
    explicit Indicator(State &s);
    ~Indicator();
    void start();
    void finish();
};
class Worker
{
    State &state_;
    std::string mode_, buffer_;
    Handle process_, write_, read_;
    HANDLE job_;
    void spawn();

  public:
    Worker(State &s, std::string mode, HANDLE job) : state_(s), mode_(std::move(mode)), job_(job)
    {
    }
    ~Worker()
    {
        reset();
    }
    void reset();
    json call(json request, unsigned timeout = 5000, bool mutation = false);
};
struct Window
{
    HWND hwnd{};
    DWORD pid{};
    uint64_t created{};
    std::string id;
    DWORD app_pid{};
    uint64_t app_created{};
    std::string aumid;
};
json foreground_information();
void request_foreground_permission(State &, HANDLE job, Window const &);
void request_session_permission(State &, HANDLE job);
class Desktop
{
    State &state_;
    HANDLE job_;
    std::map<std::string, Window> windows_;
    std::map<std::string, json> observations_;
    std::set<WORD> pressed_keys_, pressed_unicode_;
    std::set<DWORD> pressed_buttons_;
    std::mutex input_mutex_;
    json focus(Window const &, bool cursor = true, json const &activation = json::object());
    void send(std::vector<INPUT> const &);
    POINT point(json const &, Window const &);

  public:
    explicit Desktop(State &s, HANDLE job) : state_(s), job_(job)
    {
    }
    json list_windows();
    json list_apps();
    Window resolve(std::string const &);
    json launch(json const &, Worker &);
    json act(std::string const &, json const &);
    void release_input();
    void invalidate();
    void remember(json const &);
    json observation(json const &);
    json describe(Window const &);
    bool image_snapshot(std::string const &);
};
class Server
{
    State state_;
    Indicator indicator_;
    Handle job_;
    Desktop desktop_;
    Worker capture_, uia_, launch_;
    std::mutex output_mutex_, queue_mutex_;
    std::condition_variable queue_ready_;
    std::deque<json> queue_;
    bool eof_ = false, initialized_ = false;
    std::string current_request_;
    std::mutex current_mutex_;
    void write(json const &);
    void reader();
    json invoke(std::string const &, json const &);
    json observe(json const &);
    json tools();

  public:
    Server();
    ~Server();
    int run();
    json diagnostic();
    json worker_recovery_test();
    void set_language(std::string const &);
    void set_timeout(unsigned seconds);
};
json launch_request(json const &);
json capture_request(json const &);
json uia_request(json const &);
int worker_main(std::string const &);
constexpr ULONG_PTR injection_tag = 0x44434147454e54ULL;
} // namespace dc

