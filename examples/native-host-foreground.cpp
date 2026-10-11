#include "foreground_permission.hpp"

// Add to a native MCP host. Obtain both values from this connection's diagnostics
// (or from its child process handle). Invoke immediately before activate_window,
// on a foreground/user-initiated path. ERROR_ACCESS_DENIED means the host is not
// eligible: keep the rejection or use activate_window's interactive fallback.
DWORD prepare_desktop_control_activation(DWORD server_pid, uint64_t server_process_created)
{
    return dc::foreground::grant(server_pid, server_process_created);
}
