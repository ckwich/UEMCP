# Phase 1 Observability Workflow

Phase 1 makes UEMCP trustworthy before it becomes powerful. The goal is not to
change Unreal Editor state. The goal is to prove the live editor, bridge,
profile, log, readiness, and automation gates in a way an agent can follow
without guessing.

## Done Criteria

Phase 1 is done when a fresh checkout can:

- Import the Python MCP server from the pinned environment.
- Build the Unreal plugin against the installed Unreal Engine editor target.
- Launch or attach to Unreal Editor through the localhost bridge.
- Report editor identity with `uemcp_ping` and `get_editor_status`.
- Capture bounded output log entries with `get_output_log`.
- Explain editor automation readiness with `get_editor_readiness`.
- Run `diagnose_editor_automation_readiness` without starting automation.
- List and run the deterministic `UEMCP.Observability.Smoke` automation test.
- Run the active profile automation prefix through `run_profile_automation_tests`.
- Summarize the current blocker or healthy state with `summarize_observability_state`.
- Pass the live smoke script against the sample project and the active Failstate worktree.

## Agent Ladder

Use this ladder whenever an agent needs to decide what to do next:

1. `diagnose_editor_automation_readiness`
2. `run_profile_automation_tests`
3. `summarize_observability_state`
4. `get_observability_recent_events`

The first tool proves whether automation is safe to start. The second tool
runs the profile's validation path. The third tool is the compact answer to
"what is blocking us right now?" The fourth tool is for detail only, after the
summary says detail is needed.

Do not skip directly to mutation work because a profile or config says the
editor should be ready. The live readiness and profile automation gates are the
source of truth.

## Local Profile Boundary

UEMCP is intended to be shareable. Repo-specific paths, private worktrees, and
project workflow recipes should not be committed to the UEMCP repo unless they
are generic examples.

Profile lookup order:

1. `UEMCP_PROFILE_DIR/<profile>.json`
2. `.uemcp.local/profiles/<profile>.json`
3. `Python/profiles/<profile>.json`

`Python/profiles` contains shareable defaults. `.uemcp.local` is ignored by git
and is the right place for this machine's Failstate paths. `UEMCP_PROFILE_DIR`
is the right handoff point when a consuming project, such as Failstate, owns
its own profile files.

Current local Failstate work should use an ignored profile like:

```json
{
  "name": "failstate",
  "project_path": "C:/Dev/Failstate",
  "preferred_worktree_path": "C:/Dev/Failstate/.worktrees/phase1-combat-shell",
  "engine_version": "5.7",
  "content_roots": [
    "Content/Failstate",
    "Content/Failstate/Blueprints",
    "Content/Failstate/Blueprints/Blockout"
  ],
  "automation_test_prefixes": [
    "Failstate.Phase1"
  ],
  "log_categories": [
    "LogTemp",
    "LogFailstate",
    "LogAbilitySystem",
    "LogGameplayTags",
    "LogBlueprint"
  ],
  "known_maps": [],
  "notes": [
    "Local ignored profile for the active Failstate worktree.",
    "Observability tools are read-only by default.",
    "Editor-owned .uasset and .umap content must be changed through Unreal Editor APIs only."
  ]
}
```

Future repo-specific workflow tools should follow the same rule: generic
machinery lives in UEMCP; concrete Failstate recipes belong either in
Failstate's repo or in ignored `.uemcp.local` files until they are ready to
publish as examples.

## Project Pack Boundary

Phase 5 moves real project contracts into project-owned packs. For Failstate,
the active pack lives under:

```text
C:\Dev\Failstate\.worktrees\phase1-combat-shell\Tools\UEMCP
```

The smoke script auto-detects `Tools\UEMCP\profiles` next to an external
`.uproject` and sets `UEMCP_PROFILE_DIR` for the probe process. This lets
UEMCP stay shareable while Failstate owns its sentinel assets, maps, automation
prefixes, and capability expectations.

For the Phase 5 contract, see
[Phase 5 Project Pack Boundary](phase-5-project-pack-boundary.md).

## Verification Commands

Python/static gates:

```powershell
uv run --extra dev pytest -q
uv lock --check
uv run python -c "from unreal_mcp_server import mcp; print(type(mcp).__name__)"
git diff --check
```

Sample project live smoke:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Smoke-UEMCPObservability.ps1 -CloseLaunchedEditor
```

Failstate live smoke:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Smoke-UEMCPObservability.ps1 `
  -ProjectPath 'C:\Dev\Failstate\.worktrees\phase1-combat-shell\Failstate.uproject' `
  -PluginPath 'C:\Dev\UEMCP\MCPGameProject\Plugins\UnrealMCP\UnrealMCP.uplugin' `
  -ProfileDir 'C:\Dev\Failstate\.worktrees\phase1-combat-shell\Tools\UEMCP\profiles' `
  -CloseLaunchedEditor
```

## Phase 1 Closeout

Before calling Phase 1 complete:

- Run all verification commands above.
- Confirm `summarize_observability_state` reports `state: ready`, no latest
  blocker, no recommended next step, and zero unsuccessful entries during a
  passing smoke.
- Confirm `get_failstate_context` reports `profile_source.kind` as `local` or
  `environment` for real local Failstate work.
- Confirm the UEMCP git tree is clean after commits.
- Confirm adjacent Failstate worktree changes are only expected user/editor
  changes and are not staged from UEMCP.
