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

To prove the same read-only gate against Failstate without copying plugin files into the Failstate repo, attach the repo plugin through Unreal's supported `-PLUGIN=` switch:

```powershell
powershell -ExecutionPolicy Bypass -File .\Scripts\Smoke-UEMCPObservability.ps1 `
  -ProjectPath 'C:\Dev\Failstate\.worktrees\phase1-combat-shell\Failstate.uproject' `
  -PluginPath 'C:\Dev\UEMCP\MCPGameProject\Plugins\UnrealMCP\UnrealMCP.uplugin' `
  -CloseLaunchedEditor
```

If an external project has no `Plugins\UnrealMCP\UnrealMCP.uplugin` and no `-PluginPath`, the script fails before launching the editor instead of waiting for a bridge that cannot start.
