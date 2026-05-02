# Observability Tools

These tools are the first UEMCP surface. They are read-mostly and designed to prove the live Unreal Editor state before any mutating automation runs.

All observability tools return the same envelope shape:

- `ok`: true when the tool completed.
- `tool`: MCP tool name.
- `request_id`: unique call identifier.
- `started_at`, `finished_at`, `duration_ms`: timing evidence.
- `editor`: editor identity fields when available.
- `data`: typed payload for the tool.
- `warnings`: bounded non-fatal warnings.
- `error`: structured error with `category`, `message`, and `raw` when `ok` is false.

## uemcp_ping

Checks whether the Python MCP server can reach the Unreal Editor bridge. On success, returns server metadata and bridge identity.

## get_editor_status

Returns Unreal Editor identity and state:

- Plugin version.
- Engine version.
- Project name and path.
- Current map.
- PIE running state.
- Selected actor count.
- Dirty package count.

## get_output_log

Requests bounded output log data from the editor bridge. The Unreal plugin registers a thread-safe in-memory `GLog` output device during module startup and keeps the newest 2048 entries available for MCP queries.

Parameters:

- `limit`: maximum entries, clamped from 1 to 1000.
- `category`: optional Unreal log category filter.
- `verbosity`: optional verbosity filter.
- `contains`: optional substring filter.

The response includes:

- `entries`: matching log entries in chronological order, bounded by `limit`.
- `truncated`: true when more matching entries exist than were returned.
- `buffer_capacity`: current Unreal-side ring buffer capacity.
- `captured_entry_count`: total entries currently retained in the ring buffer.
- `matched_entry_count`: number of retained entries matching the requested filters before `limit` was applied.
- `filters`: the normalized filters used by the bridge.
- `warnings`: bounded non-fatal warnings.

Each entry includes `sequence`, `time_seconds`, `timestamp_utc`, `category`, `verbosity`, and `message`.

## list_automation_tests

Lists Unreal automation tests currently visible to the editor automation framework. The bridge calls `FAutomationTestFramework::LoadTestModules()` and `GetValidTestNames()` on the game thread, then returns a bounded, prefix-filtered result.

Parameters:

- `prefix`: optional dot-path prefix filter. Matching checks both `full_test_path` and Unreal's runnable `test_name`.
- `limit`: maximum tests, clamped from 1 to 1000.

The response includes:

- `tests`: matching tests, bounded by `limit`.
- `total_valid_test_count`: all valid tests reported by Unreal before filtering.
- `matched_test_count`: number of tests matching `prefix` before `limit`.
- `returned_test_count`: number of tests returned.
- `truncated`: true when more matching tests exist than were returned.
- `filters`: the normalized query.

Each test includes `display_name`, `full_test_path`, `test_name`, `tags`, `test_parameter`, `source_file`, `source_line`, `asset_path`, `open_command`, `flags`, and `participants_required`.

For Failstate, the current profile prefix is `Failstate.Phase1`.

## run_automation_test

Runs one exact Unreal automation test and returns structured execution evidence. The command accepts either the listed `full_test_path` or `test_name`; internally the bridge resolves the test through `GetValidTestNames()` and runs the framework command name.

Parameters:

- `test_name`: exact `full_test_path` or `test_name` from `list_automation_tests`.
- `timeout_seconds`: maximum latent-command drain time, clamped from 1 to 120 seconds.

The response includes:

- `test`: the resolved automation test metadata.
- `status`: `passed`, `failed`, or `timed_out`.
- `successful`: true only when the framework reports success and the timeout was not hit.
- `timed_out`: true when the latent-command drain exceeded `timeout_seconds`.
- `duration_seconds` and `reported_duration_seconds`.
- `error_count`, `warning_count`, `event_count`, `events_truncated`, and bounded `events`.

The bridge refuses to start automation tests while an editor slow task or Play In Editor session is active. This avoids Unreal's `StopTest()` assertion path and makes the failed gate explicit.

The plugin ships a deterministic smoke test named `UEMCP.Observability.Smoke`; the live smoke script lists and runs it before reporting success.

## get_failstate_context

Returns the active Failstate profile without touching Unreal state. The default profile targets:

- `C:/Dev/Failstate`
- `C:/Dev/Failstate/.worktrees/phase1-combat-shell`
- UE 5.7
- `Content/Failstate/Blueprints/Blockout`
- `Failstate.Phase1` automation tests

## Live Smoke Gate

Use the repo smoke script to prove the editor-target build, bridge listener, and read-only observability tools against a running Unreal Editor:

```powershell
powershell -ExecutionPolicy Bypass -File .\Scripts\Smoke-UEMCPObservability.ps1
```

The script defaults to `D:\Epic\UE_5.7`, builds `MCPGameProjectEditor`, launches `MCPGameProject.uproject` when the bridge is not already listening, waits for `127.0.0.1:55557`, and fails if any observability envelope is not `ok: true`, if `get_editor_status.project_path` does not match the sample project, if `get_output_log` returns no live entries, or if category/substring filtering returns entries outside the requested filter.

The script also requires `list_automation_tests(prefix="UEMCP.")` to return `UEMCP.Observability.Smoke` and `run_automation_test("UEMCP.Observability.Smoke")` to pass with zero errors.

To prove the same read-only gate against Failstate without copying plugin files into the Failstate repo, attach the repo plugin through Unreal's supported `-PLUGIN=` switch:

```powershell
powershell -ExecutionPolicy Bypass -File .\Scripts\Smoke-UEMCPObservability.ps1 `
  -ProjectPath 'C:\Dev\Failstate\.worktrees\phase1-combat-shell\Failstate.uproject' `
  -PluginPath 'C:\Dev\UEMCP\MCPGameProject\Plugins\UnrealMCP\UnrealMCP.uplugin' `
  -CloseLaunchedEditor
```

When the target project path contains Failstate, the script additionally requires `list_automation_tests(prefix="Failstate.Phase1")` to return at least one Failstate automation test and to keep every returned test inside that prefix.

If an external project has no `Plugins\UnrealMCP\UnrealMCP.uplugin` and no `-PluginPath`, the script fails before launching the editor instead of waiting for a bridge that cannot start.
