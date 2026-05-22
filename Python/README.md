# UEMCP

Observability-first Python MCP bridge for interacting with Unreal Engine through the UEMCP editor plugin.

## Setup

1. Make sure Python 3.10 through 3.13 is installed
2. Install `uv` if you haven't already:
   ```bash
   curl -LsSf https://astral.sh/uv/install.sh | sh
   ```
3. Create and activate a virtual environment:
   ```bash
   uv venv
   source .venv/bin/activate  # On Unix/macOS
   # or
   .venv\Scripts\activate     # On Windows
   ```
4. Install dependencies:
   ```bash
   uv sync --extra dev
   ```

At this point, configure your MCP client to run `uv --directory Python run unreal_mcp_server.py` from the repo root. If your client does not launch from the repo root, use the absolute path to this checkout's `Python` directory.

## Observability Tools

The first supported UEMCP tools are read-mostly:

- `uemcp_ping`
- `get_editor_status`
- `get_output_log`
- `get_level_snapshot`
- `get_pie_runtime_snapshot`
- `asset_search`
- `asset_dependencies`
- `asset_referencers`
- `asset_intake_snapshot`
- `asset_intake_diff`
- `asset_intake_write_manifest`
- `asset_import_from_disk`
- `asset_organize_plan`
- `asset_rename`
- `asset_move`
- `asset_duplicate`
- `asset_delete`
- `asset_save_packages`
- `asset_fixup_redirectors`
- `asset_prepare_for_level`
- `asset_create_blueprint_wrapper`
- `asset_place_in_level_plan`
- `asset_place_in_level`
- `asset_validate_level_placements`
- `level_list_maps`
- `level_create`
- `level_open`
- `level_save`
- `level_construction_plan`
- `level_apply_construction_plan`
- `level_validate_construction`
- `blueprint_query`
- `get_project_context`
- `get_failstate_context`
- `run_project_compatibility_gates`

These tools return structured envelopes with request IDs, timing, editor identity, warnings, and categorized errors.

`get_level_snapshot` intentionally observes the editor world. During Play In Editor, use `get_pie_runtime_snapshot` for a read-only actor/component snapshot from the active PIE runtime world; automation readiness gates still block mutation and automation while PIE is running.

## Testing Scripts

There are several scripts in the [scripts](./scripts) folder. They are useful for testing the tools and the Unreal Bridge via a direct connection. This means that you do not need to have an MCP Server running.

You should make sure you have installed dependencies and/or are running in the `uv` virtual environment in order for the scripts to work.


## Validation

```bash
uv lock --check
uv run --extra dev pytest -q
uv run python -m uemcp_neutrality
uv run python -m uemcp_tool_surface
uv run python -c "from unreal_mcp_server import mcp; print(type(mcp).__name__)"
```

For the live Unreal bridge gate, run from the repo root:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File ./Scripts/Smoke-UEMCPObservability.ps1 -CloseLaunchedEditor
```

For the deterministic asset workflow gate, run the local disk-import fixture
smoke. This path does not depend on Fab, browser, account, network, or
entitlement state:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File ./Scripts/Smoke-UEMCPAssetWorkflow.ps1 -SkipFab -CloseLaunchedEditor
```

Against the Failstate worktree, attach the UEMCP repo plugin without installing it into the game project:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File ./Scripts/Smoke-UEMCPObservability.ps1 `
  -ProjectPath 'C:\Dev\Failstate\.worktrees\phase1-combat-shell\Failstate.uproject' `
  -PluginPath 'C:\Dev\UEMCP\MCPGameProject\Plugins\UnrealMCP\UnrealMCP.uplugin' `
  -ProfileDir 'C:\Dev\Failstate\.worktrees\phase1-combat-shell\Tools\UEMCP\profiles' `
  -CloseLaunchedEditor
```

On macOS, set `UEMCP_UE_ROOT` only if Unreal is not installed at `/Users/Shared/Epic Games/UE_5.7`. The smoke script uses `RunUBT.sh`, the Mac editor executable, and a TCP socket probe when running under PowerShell Core on macOS.

## Troubleshooting

- Make sure Unreal Engine editor is loaded loaded and running before running the server.
- Check logs in `unreal_mcp.log` for detailed error information

## Development

Add Python MCP tools under `tools/`, keep read-only observability separate from mutating editor commands, and add pytest coverage before implementation.

When adding, renaming, or exposing tools, update `../Scripts/UEMCPToolSurface.Audit.json` and run `uv run python -m uemcp_tool_surface`. That gate proves every Python MCP tool and Unreal bridge command has an explicit safety classification before the repo is shared or loaded into a broker such as Toolbox.
