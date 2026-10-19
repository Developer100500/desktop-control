Desktop Control 0.1.3
Independent C++23 MCP server for controlling Windows 10 22H2 / Windows 11, x64. It controls Win32, UWP, and WinRT/WinUI applications with standard user privileges. It is not a Computer Use patch and does not use its internal protocol.

Connection
Copy `desktop-control.exe` and `desktop-control-foreground.exe` from the portable package into a single permanent folder.
Insert the full path to it into `mcp-config.example.json` and add the server to an MCP client that supports local stdio and images.
The client must launch the process in the interactive session of the current user, on an unlocked desktop. A Windows Service/Session 0 or an isolated service desktop is not suitable for this.
Call `begin_task` with a clear title. On the first task in a new MCP process, an "Allow for session" window will appear. The confirmation is valid until this connection is closed, including new tasks and window switches. Capture and automation are unavailable until permission is granted and frames are displayed. After a skipped or declined request, the user can explicitly ask to retry: then call `begin_task` with `retry_consent=true` to reopen the window without reconnecting. Automatic retries are not allowed.
Find the application via `list_apps` / `list_windows`. Do not pass HWND: use the returned `window_id`.
Get `observe`, perform one action, and use the returned fresh observation.
The server does not connect to the model itself and does not transmit data over the network. Screenshots and UIA data are received by the calling MCP client; its data transmission rules are defined by that client.

Indication
Blue bars `#4DA6FF`, 3 DIP: each connected screen.
Yellow bars `#FFD54F`, 1 DIP: the selected window, taking into account its Z-order position.
Panel at the top of the target monitor: title, task, current stage, and Stop.
The panel does not steal keyboard focus; the bars let the mouse pass through.
Language: the first preferred Windows UI language — Russian if `ru`, otherwise English. To check localization, you can pass `--language ru` or `--language en`.
Overlay windows have `WDA_EXCLUDEFROMCAPTURE`, are not included in the target catalog, and are not passed as UIA elements of the selected application.
Physical Escape and Stop pause the task. Resuming requires an explicit `resume`.
Upon completion, the frames are removed, and the panel shows the result for two seconds.
Capture, conversion, encoding, UIA, and action stages are reported by the engine itself. By default, "Waiting for AI client" is shown between calls. The server does not infer the model's reasoning from the pause.
The client can report `thinking` or `processing_screenshot` via `report_agent_status`. An up-to-date `observation_id` of the image is required for screenshot analysis. The message is valid for 30 seconds or until the next operation.

Tools and minimal loop
Example `tools/call`:
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"begin_task","arguments":{"title":"Fill test document"}}}

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

UIA actions: `invoke`, `set_value`, `toggle`, `select`, `expand`, `collapse`, `scroll`. For UIA scroll, Windows `ScrollAmount` values are used (0 = large decrement, 1 = small decrement, 2 = no scroll, 3 = large increment, 4 = small increment).
Image is MCP content of type `image` with PNG/base64. Metadata is available in `structuredContent` and duplicated as text. `coordinate_mapping_valid` indicates whether image coordinates can be used. If the WGC ContentSize and DWM frame do not match, the server does not guess the offset and rejects the coordinate action.
`blocked_regions` contains the screen coordinates of the panel: it is excluded from the image but accepts clicks in this area. Image geometry is given in physical pixels; conversion involves adding `geometry.x/y` to the image coordinate. `window_id` is linked to HWND, PID, process creation time, and application identity behind the UWP frame.
Observation IDs are valid until the next action, geometry change, or a maximum of 60 seconds. After a UIA worker restart, old references to elements are invalid. The UIA worker stores the last UIA tree; to switch between windows, call `observe(include_text=true)` again.
`outcome` distinguishes:
`delivered`: action sent; this is not a guarantee of reaching the application's goal.
`delivered_observation_failed`: action sent, but subsequent observation failed.
`rejected`: rejected before execution.
`unknown`: operation might have partially executed. Do not repeat it automatically.

Build
Requires MSVC x64, Windows SDK 10.0.22621.0 or newer, CMake/Ninja from Visual Studio; Python is only needed for tests. JSON library 3.12.0 is already included with an MIT license and checksums; no downloads at startup.
.\build.ps1 -BuildDir C:\work\desktop-control-build
For an environment where the service `cmd.exe` CMake/Ninja does not work, an equivalent direct MSVC build is provided:
.\build.ps1 -BuildDir C:\work\desktop-control-build -Direct
MSVC uses `/std:c++latest` for C++23 features; CMake sets `CXX_STANDARD 23`. CRT is statically linked. The manifest sets `asInvoker`, `uiAccess=false`, Per-Monitor DPI V2.

Tests
`tests/test_protocol.py` tests MCP without input and without creating an active task.
python tests\test_protocol.py C:\work\desktop-control-build\desktop-control.exe
C:\work\desktop-control-build\desktop-control.exe --diagnostics
C:\work\desktop-control-build\desktop-control.exe --test-worker-recovery
`desktop-control-integration.exe` is a native test client for our MCP, not the Computer Use protocol. Run it in an interactive user session. It creates its own test application, tests PNG, UIA, Unicode input, stale observations, statuses, hardware/WARP series, and drag stopping. The panel and frames are visible during the test.
Next to the integration client, you can place `integration-config.json`:
{"results_dir":"C:\\work\\desktop-control-results","cycles":100,"language":"ru","status_expiry":true,"geometry_test":true}
Logs and images of the test application are saved in this folder. For a quick check, you can reduce `cycles`. If the client errors out, the server receives EOF, stops control, and closes its own workers.
`geometry_test=true` tests moving, resizing, rejecting old coordinates, and minimizing/restoring the native fixture. `scenario="indicator_failure"` separately tests action rejection after the indicator bar is destroyed.
`scenario="acceptance"` tests Notepad, installed UWP Calculator, and Paint with a native BMP. For Paint, the client saves the initial frame and waits for `paint-actions.json` with an array of `strokes` (each stroke is an array of `{x,y}`). Coordinates must be chosen based on the saved frame. `paint_only=true` skips the first two applications. A missing Calculator is marked as `SKIP`; this is not a successful Calculator test. `scenario="uwp_probe"` tests launching, UIA, and PNG of installed DevToys if it is not already open. For x86, you can build the same fixture for x86 and specify its name in `fixture_name`.

Single permission and cursor activation
The first `begin_task` of a new MCP connection requests permission for window captures, UIA, mouse, and keyboard. Permission is stored only in the process memory and does not survive reconnection. Pressing "Cancel", Escape, Stop, canceling the MCP request, or breaking the connection during confirmation cancels the start of control.
If the user did not have time to respond, the server returns `CONSENT_TIMEOUT`; refusal or unavailability of the dialog returns `CONSENT_DENIED`. After any unsuccessful attempt, a regular `begin_task` returns `CONSENT_RETRY_REQUIRED` without a new dialog. Only after an explicit user request to retry or continue, call `begin_task` with `retry_consent=true`. This shows the window again in the same connection but does not grant permission automatically. Refusal during a retry also maintains the control ban. After confirmation, new tasks use the already granted permission, even if `retry_consent=true` is passed.
{"title":"Continue working after a missed request", "retry_consent":true}
For a separate check, set the scenario to `"consent"` in `integration-config.json`. The test client skips, rejects, and then confirms only the dialogs of its own test server. Different timeout and refusal codes, absence of automatic retry, capture ban before consent, permission reuse, and cancellation via Stop followed by an explicit retry are tested. The established MCP connection is not modified by the test.
`activate_window` first calls the standard `SetForegroundWindow`. If Windows refuses, the server raises the selected window without activation, finds an available point with `WM_NCHITTEST=HTCAPTION`, and performs a visible mouse click. Before the click, window identity, desktop availability, task state, and point belonging to the selected window are re-verified. Title bar buttons and overlapping windows are not selected. Privilege elevation, AttachThreadInput, synthetic Alt, or changes to global foreground lock settings are not applied.
If the application lacks a standard title bar, the server returns `FOCUS_POINT_REQUIRED`. The agent receives a new `observe`, selects a safe point on the window, and calls:
{"window_id":"WINDOW_ID","observation_id":"FRESH_OBSERVATION_ID","x":120,"y":40}
These are the arguments for `activate_window`, and the coordinates refer to the image. To disable the fallback click, use `cursor_fallback=false`. `foreground_prompt` is accepted for compatibility but does not open additional permission windows.
If cursor activation was needed during `click`, `scroll`, `drag`, `press_key`, or `type_text`, the result contains `focus_only=true`, `action_performed=false`, and a new observation. The agent analyzes it and executes the original action in the next call. This prevents input based on an image obtained before activation. An `outcome=unknown` result still forbids blind repetition.

Configuring approvals in Codex
The server's system window and tool approvals in Codex are different mechanisms. For a plugin from a personal directory, allow its MCP tools in `~/.codex/config.toml`:
[plugins."desktop-control@created-by-me-remote".mcp_servers."desktop-control"]
default_tools_approval_mode = "approve"
The installed local marketplace identifier may differ. The setting should apply only to this plugin; global shell permissions do not need to be changed. If individual tools already have `approval_mode="prompt"`, replace it with `approve` within the same server. The one-time server window continues to protect the start of control. Changing the configuration requires a new chat or application restart.
The regression `scenario="foreground"` tests the refusal of standard focus passing, fallback click on the title bar, repeated focus loss without a new request, a separate activation step before input, Unicode readback, permission reuse between tasks, and permission refusal without a repeated request. The first confirmation in the test is simulated by the test client only for its own MCP process; this is not a test of a human's physical click.
The native API `include/foreground_permission.hpp` and CLI `desktop-control-foreground.exe --grant SERVER_PID SERVER_PROCESS_CREATED` are retained for clients that need standard `AllowSetForegroundWindow` passing; they are no longer required for a regular agent at every activation.

Limitations
Works only on the active default desktop. Unavailable/locked desktops and elevated applications are rejected.
Focus is set via the standard API with fallback mouse activation. If neither the title bar nor an explicitly selected observed point is available, the action returns a specific error. Secure desktop and UIPI limitations are preserved.
Minimized windows are not supported for capture and input; restoration is performed by the user.
Protected content may be unavailable to WGC. A black image alone does not allow reliably determining the cause.
SDR; HDR produces a separate error.
The UIA tree depends on the application. Canvas and some custom controls require visual analysis by an external model.
Without a separate client adapter, model statuses are only available via explicit `report_agent_status`.
The Stop and Escape buttons are available as long as Windows delivers events to our user process; the secure desktop is excluded.

Source code structure
`src/server.cpp`: MCP, coordination, and argument validation.
`src/indicator.cpp`: bars, panel, DPI, local stop.
`src/capture.cpp`, `src/uia.cpp`: isolated workers.
`src/desktop.cpp`: applications, targets, coordinates, and input.
`src/foreground.cpp`, `src/foreground_helper.cpp`: focus permission passing and isolated helper.
`src/common.cpp`: RAII, localization, worker communication, and timeouts.
`resources/strings.hpp`: compiled English and Russian UI strings.
The portable package contains `diagnose.ps1`: it outputs the state as JSON; `-WorkerRecovery` additionally checks the stopping and recovery of hung workers (about 15 seconds).
Windows components are not installed or modified. Computer Use plugin code, drivers, and services are not affected.

Global CLI timeout
desktop-control.exe --timeout 300
desktop-control.exe --timeout 300 --language ru --diagnostics
`--timeout SECONDS` accepts an integer number of seconds from 1 to 86400. It replaces the timeouts for waiting for workers, frames, UIA traversal, application window appearance, title point search, indicator appearance, and session permission. The validity period of `observation_id` gets the same value: `--timeout 300` gives the agent up to five minutes to analyze the observation before the next action.
Without the parameter, the previous values are preserved: worker 5s, frame 2s, UIA traversal 3s, launch 15s, session permission 45s, observation 60s. This is a common configurable budget for the listed waits, not a hard deadline for the entire chain: a composite call can perform multiple stages.
`diagnostics` / `--diagnostics` shows the effective values in `timeouts`. Geometry and staleness checks after actions are preserved. Stop/Escape and MCP cancellation remain active.
Drag duration (up to 5000 ms), brief focus checks, cancellation polling, resource release, and agent status display duration are not increased: these are separate input limits and responsiveness mechanisms.
In the MCP client configuration, add `"--timeout", "300"` to the `args` of the EXE launch command. See `mcp-config.timeout.example.json`. Its own MCP call timeout is configured separately; the server cannot increase it with its CLI parameter.