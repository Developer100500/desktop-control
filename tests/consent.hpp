// Exercise native consent only in a separate test MCP connection; never touches the installed server.
void consent_test(std::filesystem::path directory)
{
    Client client(directory / L"desktop-control.exe", "en", 3);
    client.call("initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", json::object()},
                               {"clientInfo", {{"name", "consent-retry-test"}, {"version", "1"}}}});
    client.ignore_consent();
    auto refused = data(client.raw_tool("begin_task", {{"title", "Let this test permission expire"}}));
    ensure(refused["error"]["code"] == "CONSENT_TIMEOUT", "helper timeout is distinct from denial");
    ensure(refused["error"]["stage"] == "consent", "timeout identifies consent stage");
    ensure(client.consent_count() == 1, "initial test consent appeared");
    record("consent_timeout", refused);

    for (auto arguments : {json{{"title", "No implicit retry"}},
                           json{{"title", "No false retry"}, {"retry_consent", false}}})
    {
        refused = data(client.raw_tool("begin_task", arguments));
        ensure(refused["error"]["code"] == "CONSENT_RETRY_REQUIRED", "retry needs explicit opt-in");
        ensure(client.consent_count() == 1, "no automatic repeat after timeout");
    }
    auto state = data(client.tool("diagnostics"))["state"];
    ensure(!state["session_authorized"].get<bool>() && !state["active"].get<bool>(), "timeout grants no control");
    ensure(data(client.raw_tool("observe", {{"window_id", "not-a-real-window"}}))["error"]["code"] == "TASK_REQUIRED",
           "capture remains unavailable without permission");

    client.set_consent_response(false);
    refused = data(client.raw_tool("begin_task", {{"title", "Decline the explicit test retry"}, {"retry_consent", true}}));
    ensure(refused["error"]["code"] == "CONSENT_DENIED", "explicit retry can still be declined");
    ensure(client.consent_count() == 2, "explicit retry reopened permission");
    state = data(client.tool("diagnostics"))["state"];
    ensure(!state["session_authorized"].get<bool>() && !state["active"].get<bool>(), "decline grants no control");
    refused = data(client.raw_tool("begin_task", {{"title", "No automatic retry after decline"}}));
    ensure(refused["error"]["code"] == "CONSENT_RETRY_REQUIRED", "decline also needs explicit retry");
    ensure(client.consent_count() == 2, "no automatic repeat after decline");
    record("consent_retry_declined", refused);

    client.set_consent_response(true);
    auto task = data(client.tool("begin_task", {{"title", "Accept the explicit test retry"}, {"retry_consent", true}}));
    ensure(task["session_authorized"].get<bool>() && task["indicator_ready"].get<bool>(), "approval starts indicated task");
    ensure(client.consent_count() == 3, "approval required a new dialog");
    client.tool("end_task");
    client.tool("begin_task", {{"title", "Reuse accepted test connection"}, {"retry_consent", true}});
    ensure(client.consent_count() == 3, "authorized connection reuses permission even with retry flag");
    client.tool("end_task");
    record("consent_retry_approved", {{"count", client.consent_count()}});

    Client cancelled(directory / L"desktop-control.exe", "en", 3);
    cancelled.call("initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", json::object()},
                                  {"clientInfo", {{"name", "consent-stop-test"}, {"version", "1"}}}});
    cancelled.stop_on_consent();
    refused = data(cancelled.raw_tool("begin_task", {{"title", "Stop the test permission request"}}));
    ensure(refused["error"]["code"] == "CANCELLED" && refused["error"]["stage"] == "consent",
           "Stop cancels consent without authorizing control");
    auto stopped = data(cancelled.read().at("result"));
    ensure(!stopped["active"].get<bool>() && !stopped["session_authorized"].get<bool>(), "Stop leaves control inactive");
    refused = data(cancelled.raw_tool("begin_task", {{"title", "No automatic retry after Stop"}}));
    ensure(refused["error"]["code"] == "CONSENT_RETRY_REQUIRED", "Stop requires explicit retry");
    ensure(cancelled.consent_count() == 1, "Stop does not automatically reopen dialog");
    cancelled.set_consent_response(true);
    cancelled.tool("begin_task", {{"title", "Explicitly continue the cancelled test request"}, {"retry_consent", true}});
    ensure(cancelled.consent_count() == 2, "explicit retry after Stop reopens dialog");
    cancelled.tool("end_task");
    record("consent_retry_after_stop", {{"count", cancelled.consent_count()}});
}
