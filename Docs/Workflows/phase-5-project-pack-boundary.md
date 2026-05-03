# Phase 5 Project Pack Boundary

Phase 5 keeps UEMCP shareable while letting real projects such as Failstate own their compatibility contracts.

## Goal

UEMCP owns generic Unreal observations and validation primitives. Consuming projects own project-specific profiles, sentinel assets, maps, automation prefixes, and workflow gates.

## Boundary

- Generic UEMCP tools stay in this repo.
- Project-specific contracts live in the consuming project under `Tools/UEMCP`.
- Local machine-only overrides can still live in ignored `.uemcp.local` files.
- UEMCP scripts should prefer `UEMCP_PROFILE_DIR` and project-owned packs over repo-local private profiles.
- UEMCP smoke scripts should execute project packs through `run_project_compatibility_gates`, not through project-name-specific branches.

## Project Pack Shape

A project pack starts with:

```text
Tools/
  UEMCP/
    README.md
    profiles/
      <profile>.json
    gates/
      <gate>.json
```

The profile names content roots, automation prefixes, log categories, known maps, required UEMCP capabilities, and sentinel compatibility gates. The gate file names the project path, plugin path, and the exact smoke command for that project.

`run_project_compatibility_gates` currently supports these profile-driven gate kinds:

- `sentinel_maps`: proves the live editor is currently on an expected map.
- `sentinel_level_snapshots`: proves the current level can be observed through a bounded actor snapshot and can assert representative actor names/classes or minimum actor counts.
- `sentinel_asset_searches`: proves a content root resolves and named sentinel assets are discoverable.
- `sentinel_blueprints`: proves a representative Blueprint resolves and reports expected class identity.
- `automation_prefixes`: optionally overrides the top-level automation prefixes for compatibility validation.

When `automation_prefixes` is omitted, the runner treats top-level `automation_test_prefixes` as compatibility gates.

## Failstate Current Pack

The active Failstate Phase 1 combat shell worktree owns:

```text
C:\Dev\Failstate\.worktrees\phase1-combat-shell\Tools\UEMCP
```

The current sentinels prove:

- `Content/Failstate/Blueprints/Blockout` resolves through Asset Registry.
- `BP_FSBlockoutCover`, `BP_FSBlockoutFloor`, and `BP_FSBlockoutVisualOnly` are discoverable.
- `/Game/Failstate/Blueprints/Blockout/BP_FSBlockoutCover` resolves through `blueprint_query`.
- `BP_FSBlockoutCover` reports generated class `BP_FSBlockoutCover_C` and parent class `FSBlockoutPiece`.
- `Failstate.Phase1` automation tests are discoverable and runnable.

## Growth Rule

When Failstate adds or materially changes a system, update the Failstate pack in the same slice. Add the smallest useful sentinel proof instead of exhaustive asset lists.

Examples:

- A new enemy family adds one representative enemy Blueprint sentinel.
- A new ability system slice adds one representative Ability/Data Asset sentinel and automation prefix.
- A new map flow adds one map readiness sentinel.
- A renamed base class updates expected parent/generated-class contracts.

If UEMCP lacks a generic tool needed by a project sentinel, add the generic tool to UEMCP first, then consume it from the project pack.

## Validation

Run the public neutrality audit before committing UEMCP changes that touch tools, scripts, docs, or profiles:

```powershell
uv --directory Python run python -m uemcp_neutrality
```

The audit scans tracked files for project-specific strings and allows them only in explicit compatibility/example locations listed in `Scripts/UEMCPNeutrality.Audit.json`.

UEMCP sample smoke remains the public neutral gate. When `-PluginPath` points at a plugin under a different Unreal project, the smoke builds that plugin owner project before launching the target project so the attached plugin DLL is current:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Smoke-UEMCPObservability.ps1 -CloseLaunchedEditor
```

Failstate pack smoke uses the project-owned profile directory. The smoke auto-runs `run_project_compatibility_gates` whenever this mounted profile directory is present:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Smoke-UEMCPObservability.ps1 `
  -ProjectPath 'C:\Dev\Failstate\.worktrees\phase1-combat-shell\Failstate.uproject' `
  -PluginPath 'C:\Dev\UEMCP\MCPGameProject\Plugins\UnrealMCP\UnrealMCP.uplugin' `
  -ProfileDir 'C:\Dev\Failstate\.worktrees\phase1-combat-shell\Tools\UEMCP\profiles' `
  -CloseLaunchedEditor
```
