# Interactive Asset Workflow

Interactive asset work is a separate UEMCP lane from deterministic smoke,
readiness, and compatibility gates.

## Goal

Let an agent help with asset acquisition, import, inspection, organization,
placement, and validation while keeping the headless smoke harness independent
from Fab login, web UI, network, marketplace, and license state.

## Boundary

- Headless smoke keeps using `Scripts/Smoke-UEMCPObservability.ps1`.
- Interactive asset work uses `Scripts/Start-UEMCPInteractiveAssetWorkflow.ps1`.
- Interactive asset work must target a consuming `.uproject` by default, not
  `MCPGameProject`.
- Fab may be required for the interactive workflow, but Fab must not become a
  dependency of the public smoke harness.
- UEMCP must not perform shell-side edits to `.uasset`, `.umap`, redirectors,
  or generated Unreal asset metadata.
- Asset mutations must happen through Unreal Editor APIs or deliberate editor
  UI actions.

## What UEMCP Should Handle

UEMCP can safely own generic asset capabilities:

- Preflight a real project for interactive asset work.
- Launch the editor interactively with UEMCP attached.
- Verify Fab is installed and not explicitly disabled for that project.
- Read Asset Registry state before and after an import.
- Summarize newly added or changed assets.
- Inspect dependencies, referencers, classes, package paths, and loadability.
- Validate naming, folder placement, content roots, and project-pack sentinels.
- Create or update project-owned asset intake manifests.
- Use editor-backed APIs for imports, moves, deletes, renames, duplication,
  Blueprint edits, map placement, and package saves when those tools exist and
  report structured evidence.

Fab account actions remain interactive. The agent can guide or operate the
editor UI when permitted by the user, but it must not pretend a headless MCP
tool can bypass login, entitlement, purchase, or license flows.

## Startup Command

Use a consuming project path and, when the project does not already include the
plugin, attach the repo plugin with `-PluginPath`:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File ./Scripts/Start-UEMCPInteractiveAssetWorkflow.ps1 `
  -ProjectPath '/Users/ckwichman/Documents/Projects/Failstate/Failstate.uproject' `
  -PluginPath '/Users/ckwichman/Documents/Projects/uemcp/MCPGameProject/Plugins/UnrealMCP/UnrealMCP.uplugin' `
  -ProfileDir '/Users/ckwichman/Documents/Projects/Failstate/Tools/UEMCP/profiles' `
  -ProfileName failstate `
  -WaitForEditorReady
```

The script launches the normal editor UI. It intentionally does not add
headless flags such as null rendering or unattended startup.

## Agent Ladder

Use this ladder for asset work:

1. Start the interactive asset workflow and wait for editor-backed readiness.
2. Capture a baseline with `asset_search` for the target content root.
3. Use Fab or editor import UI to acquire assets into the project.
4. Capture a second `asset_search` for the same roots.
5. Inspect new assets with `asset_dependencies` and `asset_referencers`.
6. Run project-pack compatibility gates or project-specific asset sentinels.
7. Only then perform placement, Blueprint wiring, map saves, or other
   mutating editor-backed work.

## First-Class Future Tools

The interactive asset lane should grow in this order:

1. `asset_intake_snapshot`: read-only snapshot of selected roots and classes.
2. `asset_intake_diff`: compare two snapshots and report additions, removals,
   class changes, dependency changes, and redirector risk.
3. `asset_import_from_disk`: editor-backed import tasks for files already on
   disk, with explicit target package paths and post-import evidence.
4. `asset_organize`: editor-backed move, rename, duplicate, delete, fix-up
   redirectors, and save-package operations with dry-run support.
5. `asset_prepare_for_level`: validate meshes/materials/Blueprints for map
   placement and optionally create placement-ready Blueprint wrappers.
6. Project-pack asset intake recipes owned by consuming projects under
   `Tools/UEMCP`.

## Stop Rules

Stop and ask the user when:

- Fab prompts for login, purchase, license acceptance, or payment.
- The target project explicitly disables Fab and the user has not asked to
  change project settings.
- A mutation would affect `.uasset` or `.umap` state without an editor-backed
  API or visible editor action.
- Asset provenance is unclear enough that the user could accidentally ship
  content without a license trail.
