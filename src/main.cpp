#include "control.hpp"
#include <fcntl.h>
#include <io.h>
namespace
{
unsigned parse_timeout(std::wstring const &value)
{
    // Parse CLI timeout; value must contain integer seconds from 1 to 86400.
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::invalid_argument("--timeout requires integer seconds from 1 to 86400");
    auto seconds = std::stoull(value);
    if (seconds < 1 || seconds > 86400)
        throw std::invalid_argument("--timeout requires integer seconds from 1 to 86400");
    return static_cast<unsigned>(seconds);
}
}
int wmain(int argc, wchar_t **argv)
{
    // Run stdio MCP or a CLI mode; argc/argv provide mode, language and optional timeout seconds.
    using namespace dc;
    try
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (argc >= 3 && std::wstring(argv[1]) == L"--worker")
            return worker_main(utf8(argv[2]));
        std::string language;
        unsigned timeout = 0;
        std::wstring mode;
        for (int i = 1; i < argc; ++i)
        {
            std::wstring argument(argv[i]);
            if (argument == L"--help" || argument == L"-h")
            {
                std::cout << "Usage: desktop-control.exe [--timeout SECONDS] [--language ru|en]"
                             " [--diagnostics|--test-worker-recovery]\n"
                             "--timeout: common internal wait budget and observation lifetime, 1..86400 seconds.\n"
                             "Without it, existing operation-specific defaults are preserved.\n"
                             "MCP client timeouts and input safety limits are configured separately.\n"
							 "The program currently supports two languages: English and Russian.";
                return 0;
            }
            if (argument == L"--timeout" || argument == L"--language")
            {
                if (++i >= argc)
                    throw std::invalid_argument(utf8(argument) + " requires a value");
                if (argument == L"--timeout")
                {
                    if (timeout)
                        throw std::invalid_argument("--timeout was specified twice");
                    timeout = parse_timeout(argv[i]);
                }
                else
                {
                    if (!language.empty())
                        throw std::invalid_argument("--language was specified twice");
                    language = utf8(argv[i]);
                    if (language != "ru" && language != "en")
                        throw std::invalid_argument("--language must be ru or en");
                }
            }
            else if (argument == L"--diagnostics" || argument == L"--test-worker-recovery")
            {
                if (!mode.empty())
                    throw std::invalid_argument("Select only one CLI mode");
                mode = argument;
            }
            else
                throw std::invalid_argument("Unknown option: " + utf8(argument));
        }
        Server server;
        if (timeout)
            server.set_timeout(timeout);
        if (!language.empty())
            server.set_language(language);
        if (mode == L"--diagnostics")
        {
            std::cout << server.diagnostic().dump(2) << '\n';
            return 0;
        }
        if (mode == L"--test-worker-recovery")
        {
            std::cout << server.worker_recovery_test().dump(2) << '\n';
            return 0;
        }
        return server.run();
    }
    catch (winrt::hresult_error const &e)
    {
        std::cerr << "desktop-control HRESULT: " << winrt::to_string(e.message()) << '\n';
        return 1;
    }
    catch (std::exception const &e)
    {
        std::cerr << "desktop-control: " << e.what() << '\n';
        return 1;
    }
}
