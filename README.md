# Desktop Control 0.1.3

C++23 MCP server for Windows 10 22H2 / Windows 11, x64. Controls Win32, UWP, and WinRT/WinUI apps with standard user privileges through its own protocol.

## Connection

Keep `desktop-control.exe` and `desktop-control-foreground.exe` in one permanent folder. Set the full server path in `mcp-config.example.json` and add it to an MCP client supporting local stdio and images. Run in the current user's interactive session on an unlocked desktop; services and Session 0 are unsupported.

1. Call `begin_task` with a clear title and obtain session permission.
2. Find an app with `list_apps` / `list_windows`; use `window_id`, not HWND.
3. Call `observe`, perform one action, and inspect the fresh observation.
4. Call `end_task` when finished.

The first task opens an "Allow for session" dialog covering capture, UIA, mouse, and keyboard until the connection closes. Control remains blocked until consent and visible indicators. After a failed attempt, retry only on explicit user request with `retry_consent=true`. Failures return `CONSENT_TIMEOUT` or `CONSENT_DENIED`; subsequent ordinary calls return `CONSENT_RETRY_REQUIRED`.

The server makes no model or network connections. The MCP client receives screenshots and UIA data and controls their transmission.

## Indicators and stopping

Blue screen borders and a yellow target border show active control. A focus-free panel displays the task, stage, and Stop button. Physical Escape or Stop pauses control; resuming requires explicit `resume`. Indicators disappear at completion; the result remains for two seconds. Overlays are excluded from capture, target lists, and UIA.

UI language follows Windows: Russian for `ru`, English otherwise; override with `--language ru` or `--language en`. Engine stages are reported automatically. Between calls, the panel shows "Waiting for AI client". Clients can report `thinking` or `processing_screenshot` with `report_agent_status`; screenshot analysis requires a current `observation_id`. Status expires after 30 seconds or the next operation.

## Tools

Example `tools/call`:

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"begin_task","arguments":{"title":"Fill test document"}}}
```

| Tool | Main arguments |
| --- | --- |
| begin_task | `title`, optional `retry_consent=false`: repeat dialog only upon explicit user request |
| end_task, list_apps, list_windows, stop, resume, diagnostics | None |
| launch_app | `app_id`: absolute path to `.exe` or AUMID; for Win32 — optional array of string `arguments` |
| activate_window | `window_id`, `cursor_fallback=true`; for own title bar: current `observation_id`, `x`, `y`; `foreground_prompt` kept as an ignored compatibility argument |
| close_window | `window_id` |
| observe | `window_id`; `include_screenshot=true`, `include_text=false`, `backend="auto"` or `"warp"` |
| element_action | `window_id`, `observation_id`, `element_id`, `action`; for `set_value` — `value` |
| click | `window_id`, `observation_id`, `x`, `y`; `button`, `count=1/2` |
| scroll | Same coordinates; `vertical`, `horizontal`: 120 units per wheel notch |
| drag | `window_id`, `observation_id`, `points=[{"x":10,"y":20},…]`, `duration_ms` ≤ 5000 |
| press_key | `window_id`, `key="Ctrl+S"` or `keys=["Ctrl","S"]` |
| type_text | `window_id`, `text`: Unicode, max 32768 UTF-16 units |
| report_agent_status | `task_id`, `phase`; for `processing_screenshot` — `observation_id` |

UIA actions: `invoke`, `set_value`, `toggle`, `select`, `expand`, `collapse`, `scroll`. UIA scroll uses Windows `ScrollAmount`: 0 = large decrement, 1 = small decrement, 2 = none, 3 = large increment, 4 = small increment.

Observations contain PNG/base64 MCP image content and metadata in `structuredContent`, also duplicated as text. Use image coordinates only when `coordinate_mapping_valid=true`; screen coordinates add `geometry.x/y`. Avoid panel coordinates listed in `blocked_regions`. Geometry mismatches reject coordinate actions. Window identities include HWND, PID, creation time, and the app behind a UWP frame.

Observations expire after an action, geometry change, or 60 seconds by default. UIA worker restarts invalidate element references; switching windows requires a fresh `observe(include_text=true)`.

Action `outcome`: `delivered` means input was sent, not that the goal was reached; `delivered_observation_failed` means the follow-up capture failed; `rejected` means no execution; `unknown` means possible partial execution and must not be retried automatically.

## Window activation

`activate_window` tries `SetForegroundWindow`, then a visible click on a verified, unobstructed title bar. It rechecks window identity and task state before clicking. Set `cursor_fallback=false` to disable this fallback.

If no title bar is available, `FOCUS_POINT_REQUIRED` requests a fresh observation and an explicit safe point:

```json
{"window_id":"WINDOW_ID","observation_id":"FRESH_OBSERVATION_ID","x":120,"y":40}
```

If input first requires cursor activation, the result returns `focus_only=true`, `action_performed=false`, and a fresh observation. Inspect it before sending the original action again. `foreground_prompt` is ignored for compatibility. The native foreground permission API and helper CLI remain available for clients using `AllowSetForegroundWindow`.

## Build and tests

Requires MSVC x64, Windows SDK 10.0.22621.0+, and Visual Studio CMake/Ninja. Python is needed only for protocol tests. JSON 3.12.0 is bundled under MIT; startup requires no downloads.

```powershell
.\build.ps1 -BuildDir C:\work\desktop-control-build
# Direct MSVC alternative if CMake/Ninja cannot run cmd.exe:
.\build.ps1 -BuildDir C:\work\desktop-control-build -Direct
python tests\test_protocol.py C:\work\desktop-control-build\desktop-control.exe
C:\work\desktop-control-build\desktop-control.exe --diagnostics
C:\work\desktop-control-build\desktop-control.exe --test-worker-recovery
```

The build uses C++23, static CRT, `asInvoker`, `uiAccess=false`, and Per-Monitor DPI V2. Protocol tests create no active task or input. Run `desktop-control-integration.exe` in an interactive session for PNG, UIA, Unicode, stale observation, status, WGC/WARP, geometry, and Stop tests. Indicators are visible during integration tests.

Place `integration-config.json` beside the test client to configure logs and scenarios:

```json
{"results_dir":"C:\\work\\desktop-control-results","cycles":100,"language":"ru","status_expiry":true,"geometry_test":true}
```

Additional scenarios: `consent`, `foreground`, `indicator_failure`, `acceptance` (Notepad, Calculator, Paint), and `uwp_probe` (DevToys). Consent tests simulate dialogs only for their own server. Paint acceptance uses `paint-actions.json` strokes chosen from the saved frame; `paint_only=true` skips other apps. Missing Calculator is `SKIP`. Use `fixture_name` for an alternate fixture, including x86. Logs and images go to `results_dir`; reduce `cycles` for a quick check.

## Codex tool approvals

Server consent and Codex tool approvals are separate. For a personal plugin, configure its MCP server in `~/.codex/config.toml` (the marketplace identifier may differ):

```toml
[plugins."desktop-control@created-by-me-remote".mcp_servers."desktop-control"]
default_tools_approval_mode = "approve"
```

Replace any per-tool `approval_mode="prompt"` overrides with `approve` for this server. Start a new chat or restart the app afterward. Session consent still applies.

## Timeouts

```powershell
desktop-control.exe --timeout 300 --language ru --diagnostics
```

`--timeout SECONDS` (1–86400) sets worker, frame, UIA, launch, title-point, indicator, consent, and observation budgets. It applies per stage, not to the whole call. Defaults: worker 5s, frame 2s, UIA 3s, launch 15s, consent 45s, observation 60s. Inspect effective values in diagnostics under `timeouts`.

Stop/Escape, cancellation, geometry checks, the 5000 ms drag limit, and status expiry remain unchanged. Add `"--timeout", "300"` to client launch `args`; see `mcp-config.timeout.example.json`. Configure the client's MCP call timeout separately.

## Limitations and source

Requires the active, unlocked default desktop. Elevated apps and minimized-window capture/input are unsupported. Secure desktop and UIPI restrictions apply. Protected content may be unavailable; black frames alone do not identify the cause. SDR only; HDR returns an error. UIA support depends on the app; canvas and custom controls may require visual analysis. Stop/Escape depends on Windows delivering events to the server process.

Sources: `src/server.cpp` (MCP), `indicator.cpp` (panel and borders), `capture.cpp` / `uia.cpp` (workers), `desktop.cpp` (apps and input), `foreground.cpp` / `foreground_helper.cpp` (focus), `common.cpp` (shared utilities), and `resources/strings.hpp` (English/Russian UI).

The portable package includes `diagnose.ps1` for JSON diagnostics; `-WorkerRecovery` also tests hung-worker recovery (about 15 seconds). No Windows components, drivers, services, or Computer Use plugin files are installed or modified.
