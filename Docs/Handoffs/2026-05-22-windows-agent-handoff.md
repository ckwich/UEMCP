# Windows Agent Handoff - 2026-05-22

## Repo State

- Repo: `/Users/ckwichman/Documents/Projects/uemcp`
- Source branch on Mac: `codex/observability-foundation`
- Pre-handoff implementation head: `37130ec fix: close bridge sockets without double destroy`
- Intended pushed integration target: `main`
- Remote: `origin` (`https://github.com/ckwich/UEMCP.git`)

The Mac branch was `43` commits ahead of `origin/main` before this handoff
recap was added. Treat the pushed `main` branch as the source of truth after
merge/push.

## What Landed

- Interactive asset workflow split from normal automation so Fab/browser/editor
  acquisition can be handled deliberately without blocking core MCP use.
- Generic `level_*` tools for map discovery, create/open/save, construction
  planning, batch construction, and construction validation.
- UE 5.7 C++ bridge routing and command handlers for editor-owned map lifecycle
  and actor construction.
- Project-neutral `map_recipes` profile gates:
  - `map_recipe_current_map`
  - `map_recipe_expected_actors`
  - `map_recipe_min_actor_count`
- Tool-surface audit entries, docs, project recipe examples, and a live smoke
  script for map construction.
- Follow-up fixes on top of the level workflow:
  - `c8a704f fix: avoid level workflow unity helper collisions`
  - `37130ec fix: close bridge sockets without double destroy`

## Important Files

- `Python/tools/level_workflow_tools.py`
- `Python/tools/observability_tools.py`
- `Python/unreal_mcp_server.py`
- `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPLevelWorkflowCommands.cpp`
- `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/Commands/UnrealMCPLevelWorkflowCommands.h`
- `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/UnrealMCPBridge.cpp`
- `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/UnrealMCPBridge.h`
- `Scripts/Smoke-UEMCPLevelWorkflow.ps1`
- `Docs/Workflows/interactive-asset-workflow.md`
- `Docs/Workflows/level-map-construction.md`
- `Docs/Workflows/project-map-recipes.example.json`

## Validation To Re-Run On Windows

Run these from the repo root after pulling `main`:

```powershell
uv --directory Python run pytest -q
uv --directory Python run python -m uemcp_tool_surface
uv --directory Python run python -m uemcp_neutrality
```

Then rebuild the Unreal editor target with the Windows UE 5.7 install:

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" MCPGameProjectEditor Win64 Development -Project="$PWD\MCPGameProject\MCPGameProject.uproject" -NoHotReloadFromIDE
```

Finally run the level workflow smoke:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Smoke-UEMCPLevelWorkflow.ps1 -CloseLaunchedEditor
```

Expected marker:

```text
LEVEL_WORKFLOW_SMOKE_OK
```

## Mac Validation Already Completed

- `uv --directory Python run pytest -q` passed.
- `uv --directory Python run python -m uemcp_tool_surface` returned
  `TOOL_SURFACE_OK`.
- `uv --directory Python run python -m uemcp_neutrality` returned
  `NEUTRALITY_OK`.
- UE 5.7 Mac `RunUBT.sh MCPGameProjectEditor Mac Development` succeeded.
- `Scripts/Smoke-UEMCPLevelWorkflow.ps1 -CloseLaunchedEditor` emitted
  `LEVEL_WORKFLOW_SMOKE_OK`.
- Post-smoke cleanup left no `.umap`, `.uasset`, or redirector files under
  `MCPGameProject/Content`.

## Known Local-Only Residue

The Mac workspace still had an unrelated untracked duplicate plan file at
`Docs/superpowers/plans/2026-05-22-uemcp-interactive-asset-workflows 2.md`.
It was intentionally not staged for the implementation commit and should not be
treated as part of the pushed handoff unless a future agent chooses to recover
or delete it.

## Next Useful Agent Slice

On Windows, prove the merged `main` branch against the Windows UE 5.7 editor
install, then exercise the interactive asset workflow with Fab enabled. Keep
asset acquisition workflows separate from normal readiness/automation workflows,
and keep project-specific maps or sentinels in consuming project packs instead
of adding them to the public UEMCP surface.
