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

Requests bounded output log data from the editor bridge.

Parameters:

- `limit`: maximum entries, clamped from 1 to 1000.
- `category`: optional Unreal log category filter.
- `verbosity`: optional verbosity filter.
- `contains`: optional substring filter.

The current Unreal-side implementation returns the stable response shape with an explicit warning that historical log capture is not fully wired yet.

## get_failstate_context

Returns the active Failstate profile without touching Unreal state. The default profile targets:

- `C:/Dev/Failstate`
- `C:/Dev/Failstate/.worktrees/phase1-combat-shell`
- UE 5.7
- `Content/Failstate/Blueprints/Blockout`
- `Failstate.Phase1` automation tests
