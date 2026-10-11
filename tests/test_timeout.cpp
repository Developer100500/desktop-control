#include "control.hpp"
#include <stdexcept>
namespace
{
void expect(bool passed, char const *message)
{
    // Fail if passed is false; message describes the violated behavior.
    if (!passed)
        throw std::runtime_error(message);
}
std::string validate_snapshot(dc::State &state, int64_t age)
{
    // Validate an artificial snapshot aged by age milliseconds; state supplies its configured lifetime.
    dc::Desktop desktop(state, nullptr);
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        dc::Clock::now().time_since_epoch()).count();
    desktop.remember({{"observation_id", "test-snapshot"}, {"window_id", "nonexistent"},
                      {"steady_ms", now - age}, {"geometry", nullptr}});
    try
    {
        desktop.observation({{"observation_id", "test-snapshot"}, {"window_id", "nonexistent"}});
    }
    catch (dc::Error const &error)
    {
        return error.code;
    }
    throw std::runtime_error("Nonexistent window was accepted");
}
}
int wmain(int argc, wchar_t **argv)
{
    // Test without desktop input; argc/argv also route this test executable's worker subprocess.
    using namespace dc;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (argc >= 3 && std::wstring(argv[1]) == L"--worker")
            return worker_main(utf8(argv[2]));
        State state;
        expect(state.timeout_ms(5000) == 5000, "Default worker budget changed");
        expect(validate_snapshot(state, 61000) == "STALE_OBSERVATION", "Default snapshot did not expire");
        state.timeout_override_ms = 300000;
        expect(state.timeout_ms(5000) == 300000, "Worker override not applied");
        expect(validate_snapshot(state, 61000) == "UNKNOWN_WINDOW", "Snapshot expired at old 60-second limit");
        expect(validate_snapshot(state, 301000) == "STALE_OBSERVATION", "Override snapshot did not expire");
        state.timeout_override_ms = 1000;
        Handle job(CreateJobObjectW(nullptr, nullptr));
        expect(bool(job), "Cannot create worker job");
        Worker worker(state, "capture", job.value);
        auto started = Clock::now();
        try
        {
            worker.call({{"operation", "test_hang"}, {"hwnd", uint64_t(0)}}, 5000);
            throw std::runtime_error("Hanging worker did not time out");
        }
        catch (Error const &error)
        {
            expect(error.code == "WORKER_TIMEOUT", "Hanging worker failed for another reason");
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        expect(elapsed >= 900 && elapsed < 4000, "Worker still used old five-second limit");
        std::cout << "PASS: default/overridden snapshot expiry and actual worker timeout (" << elapsed << " ms)\n";
        return 0;
    }
    catch (std::exception const &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
