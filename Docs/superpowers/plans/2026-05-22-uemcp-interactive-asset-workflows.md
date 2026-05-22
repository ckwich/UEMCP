# UEMCP Interactive Asset Workflows Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a complete, dependable interactive asset lane so agents can help acquire, import, inspect, organize, place, and validate Unreal assets without making headless smoke depend on Fab, auth, marketplace, browser, or network state.

**Architecture:** Keep three lanes separate: deterministic headless smoke, interactive acquisition, and editor-backed asset mutation. Public UEMCP owns generic tooling and evidence contracts; consuming projects own asset intent, content roots, recipes, license notes, maps, and sentinels through `Tools/UEMCP` packs.

**Tech Stack:** PowerShell launcher scripts, Python MCP tool schemas, Unreal Editor C++ bridge commands, Asset Registry, AssetTools, EditorAssetLibrary, EditorLoadingAndSavingUtils, project-owned `Tools/UEMCP` profiles and recipes.

---

## Planning Rules

- Do not re-enable Fab in `MCPGameProject`; the harness remains deterministic.
- Do not shell-edit `.uasset`, `.umap`, redirectors, generated asset metadata, or saved package state.
- Every mutating tool must support `dry_run` where practical and must report preflight evidence, intended package paths, changed packages, and post-operation evidence.
- Treat Fab login, entitlement, purchase, license acceptance, and payment as interactive stop points. The agent may guide or operate visible editor UI when permitted, but must not imply UEMCP can bypass those flows.
- Keep concrete asset choices in consuming project packs. UEMCP may provide example schemas, but should not hard-code Failstate assets or paths in public tools.

## Slice Map

| Slice | Name | Scope | Depends On | Commit Shape |
| --- | --- | --- | --- | --- |
| 0 | Boundary and Launcher | Interactive lane exists, Fab preflight, docs, tests | Current repo | `feat: add interactive asset workflow lane` |
| 1 | Read-Only Intake Snapshot | Capture root/class asset inventory with dependencies | Slice 0 | `feat: add asset intake snapshots` |
| 2 | Snapshot Diff and Manifest | Compare before/after imports and write optional manifests | Slice 1 | `feat: add asset intake diffs` |
| 3 | Fab-Assisted Import Protocol | Human/agent Fab UI acquisition with automated pre/post checks | Slice 2 | `feat: document fab import protocol` |
| 4 | Disk Import API | Editor-backed import from local files | Slice 2 | `feat: add editor asset import tool` |
| 5 | Asset Organization API | Move, rename, duplicate, delete, save, redirector fix-up | Slice 2 | `feat: add asset organization tools` |
| 6 | Asset Quality Gates | Validate classes, paths, dependencies, materials, textures, Nanite/collision/basic loadability | Slices 1-5 | `feat: add asset quality gates` |
| 7 | Placement Preparation | Convert inspected assets into placement-ready plans or wrappers | Slice 6 | `feat: prepare assets for placement` |
| 8 | Map Placement Workflow | Place assets into maps through editor APIs with save evidence | Slice 7 | `feat: add asset placement workflow` |
| 9 | Project-Pack Recipes | Consuming projects define asset recipes and post-import gates | Slices 1-8 | `feat: add project asset recipes` |
| 10 | End-to-End Asset Smoke | Interactive smoke guide plus optional automated disposable import test | Slices 1-9 | `test: add asset workflow smoke` |

## Slice 0: Boundary and Launcher

**Status:** Done in commit `cfd619a feat: add interactive asset workflow lane`.

**Files:**
- Created: `Scripts/Start-UEMCPInteractiveAssetWorkflow.ps1`
- Created: `Docs/Workflows/interactive-asset-workflow.md`
- Created: `Python/tests/test_asset_workflow.py`
- Created: `Docs/superpowers/plans/2026-05-22-uemcp-interactive-asset-workflows.md`
- Modified: `Docs/README.md`

**Validation Already Run:**

```bash
uv --directory Python run pytest
uv --directory Python run python -m uemcp_neutrality
git diff --check
pwsh -NoProfile -File Scripts/Start-UEMCPInteractiveAssetWorkflow.ps1 -AllowHarnessProject -SkipBuild -SkipLaunch -NoFabRequirement
```

## Slice 1: Read-Only Intake Snapshot

**Status:** Done in the first implementation wave.

**Goal:** Add a rich read-only snapshot tool that records asset inventory before and after acquisition without loading or mutating packages.

**Files:**
- Create: `Python/tools/asset_workflow_tools.py`
- Create: `Python/tests/test_asset_workflow_tools.py`
- Modify: `Python/unreal_mcp_server.py`
- Modify: `Python/uemcp_tool_surface.py`
- Modify: `Scripts/UEMCPToolSurface.Audit.json`
- Create: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/Commands/UnrealMCPAssetWorkflowCommands.h`
- Create: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPAssetWorkflowCommands.cpp`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/UnrealMCPBridge.cpp`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/UnrealMCPBridge.h`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/UnrealMCP.Build.cs`

**Tool Contract:**

```json
{
  "tool": "asset_intake_snapshot",
  "params": {
    "roots": ["/Game/Fab", "/Game/Environment"],
    "classes": ["StaticMesh", "MaterialInstanceConstant", "Texture2D", "Blueprint"],
    "include_dependencies": true,
    "include_referencers": false,
    "include_tags": true,
    "limit": 5000
  }
}
```

**Response Shape:**

```json
{
  "snapshot_id": "asset-snapshot-<uuid>",
  "roots": ["/Game/Fab"],
  "filters": {
    "classes": ["StaticMesh"],
    "include_dependencies": true,
    "include_referencers": false,
    "include_tags": true,
    "limit": 5000
  },
  "asset_count": 1,
  "truncated": false,
  "asset_registry_loading": false,
  "assets": [
    {
      "asset_name": "SM_Crate",
      "object_path": "/Game/Fab/Crate/SM_Crate.SM_Crate",
      "package_name": "/Game/Fab/Crate/SM_Crate",
      "package_path": "/Game/Fab/Crate",
      "asset_class": "StaticMesh",
      "asset_class_path": "/Script/Engine.StaticMesh",
      "tags": {},
      "dependencies": [],
      "referencers": []
    }
  ],
  "warnings": []
}
```

**Implementation Steps:**

- [x] Add failing schema tests in `Python/tests/test_asset_workflow_tools.py` that assert `asset_intake_snapshot` is registered, requires `roots`, supports optional classes/dependencies/referencers/tags/limit, and exposes no `ctx`.
- [x] Add failing contract tests that assert `UnrealMCPBridge.cpp` routes `asset_intake_snapshot` to `FUnrealMCPAssetWorkflowCommands`.
- [x] Implement Python builder in `Python/tools/asset_workflow_tools.py` using the existing bridge-call and envelope patterns from `Python/tools/editor_tools.py`.
- [x] Register the new tools from `Python/unreal_mcp_server.py`.
- [x] Add `FUnrealMCPAssetWorkflowCommands` and implement `HandleAssetIntakeSnapshot` using `IAssetRegistry`.
- [x] Clamp `limit` to a documented maximum such as 10000 and return `truncated: true` when exceeded.
- [x] Include `asset_registry_loading` and a warning when the registry is still scanning.
- [x] Update `Scripts/UEMCPToolSurface.Audit.json` and tool-surface tests with category `asset_workflow_observation`, safety `read_only`.
- [x] Run validation:

```bash
uv --directory Python run pytest tests/test_asset_workflow_tools.py tests/test_mcp_tool_contract.py tests/test_tool_surface_audit.py
'/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUBT.sh' MCPGameProjectEditor Mac Development -Project="$PWD/MCPGameProject/MCPGameProject.uproject" -Progress -NoEngineChanges -NoHotReloadFromIDE
uv --directory Python run pytest
```

## Slice 2: Snapshot Diff and Manifest

**Status:** Done in the first implementation wave.

**Goal:** Compare two snapshots and optionally write a JSON manifest that records what landed during asset intake.

**Files:**
- Modify: `Python/tools/asset_workflow_tools.py`
- Modify: `Python/tests/test_asset_workflow_tools.py`
- Create: `Python/uemcp_asset_intake.py`
- Create: `Python/tests/test_asset_intake_manifest.py`
- Modify: `Docs/Workflows/interactive-asset-workflow.md`

**Tool Contracts:**

```json
{
  "tool": "asset_intake_diff",
  "params": {
    "before": {"snapshot_id": "before", "assets": []},
    "after": {"snapshot_id": "after", "assets": []},
    "include_unchanged": false
  }
}
```

```json
{
  "tool": "asset_intake_write_manifest",
  "params": {
    "diff": {"added": [], "removed": [], "changed": []},
    "output_path": "Tools/UEMCP/asset-intake/2026-05-22-crate-pack.json",
    "notes": ["Imported through Fab UI after user accepted license prompt."]
  }
}
```

**Implementation Steps:**

- [x] Add pure-Python tests for diff behavior: added, removed, changed class, changed dependency, unchanged suppression.
- [x] Implement `Python/uemcp_asset_intake.py` with `normalize_snapshot`, `diff_snapshots`, and `write_manifest`.
- [x] Add tool builders for `asset_intake_diff` and `asset_intake_write_manifest`.
- [x] Make manifest writes refuse paths outside `Tools/UEMCP/asset-intake` unless `allow_custom_output_path` is true.
- [x] Ensure manifests contain timestamp, active profile, project path if available, source snapshot IDs, changed package names, and notes.
- [x] Run validation:

```bash
uv --directory Python run pytest tests/test_asset_workflow_tools.py tests/test_asset_intake_manifest.py
uv --directory Python run pytest
```

## Slice 3: Fab-Assisted Import Protocol

**Goal:** Make Fab UI acquisition operationally dependable even though Fab login/license flows remain interactive.

**Files:**
- Modify: `Scripts/Start-UEMCPInteractiveAssetWorkflow.ps1`
- Modify: `Docs/Workflows/interactive-asset-workflow.md`
- Create: `Docs/Workflows/fab-assisted-import.md`
- Modify: `Python/tests/test_asset_workflow.py`

**Protocol:**

1. Start interactive asset workflow with the consuming project.
2. Verify editor readiness.
3. Run `asset_intake_snapshot` for expected roots.
4. User/agent uses Fab UI to acquire/import assets.
5. Stop for login, entitlement, purchase, or license acceptance prompts.
6. Run another `asset_intake_snapshot`.
7. Run `asset_intake_diff`.
8. Write a project-owned manifest.
9. Run project-pack gates.

**Implementation Steps:**

- [ ] Add `-ExpectedAssetRoots` to the launcher as a string array for preflight output only.
- [ ] Add tests proving the launcher does not claim to automate Fab purchase/license actions.
- [ ] Document visible UI stop points and allowed agent actions.
- [ ] Add a checklist for recording asset source URL/name/license notes in the manifest.
- [ ] Run validation:

```bash
uv --directory Python run pytest tests/test_asset_workflow.py
uv --directory Python run python -m uemcp_neutrality
```

## Slice 4: Editor-Backed Disk Import API

**Goal:** Import files already on disk through Unreal Editor APIs with explicit destination paths and post-import evidence.

**Files:**
- Modify: `Python/tools/asset_workflow_tools.py`
- Modify: `Python/tests/test_asset_workflow_tools.py`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPAssetWorkflowCommands.cpp`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/Commands/UnrealMCPAssetWorkflowCommands.h`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/UnrealMCPBridge.cpp`

**Tool Contract:**

```json
{
  "tool": "asset_import_from_disk",
  "params": {
    "source_files": ["/Users/example/Downloads/SM_Crate.fbx"],
    "destination_path": "/Game/Imported/Crates",
    "replace_existing": false,
    "save_imported_assets": false,
    "dry_run": true
  }
}
```

**Implementation Steps:**

- [ ] Add schema tests for required `source_files` and `destination_path`, optional `replace_existing`, `save_imported_assets`, and `dry_run`.
- [ ] Validate destination paths must start with `/Game/`.
- [ ] Dry-run returns normalized source file list, destination packages, conflicts, missing files, and no mutation.
- [ ] Editor implementation uses `UAssetImportTask` and `FAssetToolsModule`.
- [ ] Post-import response includes imported assets, failed files, warnings, dirty packages, and save status.
- [ ] Do not auto-save by default; require `save_imported_assets: true`.
- [ ] Run validation:

```bash
uv --directory Python run pytest tests/test_asset_workflow_tools.py
'/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUBT.sh' MCPGameProjectEditor Mac Development -Project="$PWD/MCPGameProject/MCPGameProject.uproject" -Progress -NoEngineChanges -NoHotReloadFromIDE
```

## Slice 5: Asset Organization API

**Goal:** Provide safe editor-backed move, rename, duplicate, delete, save, and redirector fix-up primitives.

**Files:**
- Modify: `Python/tools/asset_workflow_tools.py`
- Modify: `Python/tests/test_asset_workflow_tools.py`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPAssetWorkflowCommands.cpp`
- Modify: `Docs/Tools/editor_tools.md`
- Modify: `Scripts/UEMCPToolSurface.Audit.json`

**Tools:**

- `asset_organize_plan`: dry-run only, validates requested operations.
- `asset_rename`
- `asset_move`
- `asset_duplicate`
- `asset_delete`
- `asset_save_packages`
- `asset_fixup_redirectors`

**Implementation Steps:**

- [ ] Add tool-surface audit entries with safety `asset_mutation`.
- [ ] Add tests requiring `dry_run` support for plan generation.
- [ ] Implement operations with `UEditorAssetLibrary`, `FAssetToolsModule`, and editor save helpers.
- [ ] Require exact package names for mutating calls; do not accept broad wildcards for delete.
- [ ] Return changed packages and post-operation `asset_intake_snapshot` references when requested.
- [ ] Run validation:

```bash
uv --directory Python run pytest tests/test_asset_workflow_tools.py tests/test_tool_surface_audit.py
uv --directory Python run python -m uemcp_neutrality
```

## Slice 6: Asset Quality Gates

**Goal:** Let project packs validate imported assets before they become gameplay or map dependencies.

**Files:**
- Modify: `Python/uemcp_observability.py`
- Modify: `Python/tests/test_observability_contract.py`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPAssetWorkflowCommands.cpp`
- Modify: `Docs/Workflows/phase-5-project-pack-boundary.md`
- Modify: `Docs/Workflows/interactive-asset-workflow.md`

**Gate Kinds:**

- `asset_recipe_roots`
- `asset_recipe_classes`
- `asset_recipe_naming`
- `asset_recipe_dependencies`
- `asset_recipe_materials`
- `asset_recipe_mesh_readiness`
- `asset_recipe_blueprint_readiness`

**Implementation Steps:**

- [ ] Add profile schema tests for `asset_recipes`.
- [ ] Implement generic gate evaluation from profile-owned recipe JSON.
- [ ] Add C++ commands only where Asset Registry metadata is insufficient.
- [ ] Keep project-specific expected names and roots in profile pack files.
- [ ] Run validation:

```bash
uv --directory Python run pytest tests/test_observability_contract.py tests/test_asset_workflow_tools.py
uv --directory Python run python -m uemcp_neutrality
```

## Slice 7: Placement Preparation

**Goal:** Convert inspected assets into safe placement plans or wrapper Blueprints without guessing.

**Files:**
- Modify: `Python/tools/asset_workflow_tools.py`
- Modify: `Python/tests/test_asset_workflow_tools.py`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPAssetWorkflowCommands.cpp`
- Modify: `Docs/Workflows/interactive-asset-workflow.md`

**Tools:**

- `asset_prepare_for_level`
- `asset_create_blueprint_wrapper`

**Implementation Steps:**

- [ ] Add tests for static mesh placement readiness fields: bounds, collision presence, material slots, Nanite flag if available, scale hints, and warnings.
- [ ] Add Blueprint wrapper dry-run with generated package name and component plan.
- [ ] Implement wrapper creation through existing Blueprint/component APIs or shared C++ helpers.
- [ ] Require explicit target package path under `/Game/`.
- [ ] Run validation:

```bash
uv --directory Python run pytest tests/test_asset_workflow_tools.py tests/test_mcp_tool_contract.py
'/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUBT.sh' MCPGameProjectEditor Mac Development -Project="$PWD/MCPGameProject/MCPGameProject.uproject" -Progress -NoEngineChanges -NoHotReloadFromIDE
```

## Slice 8: Map Placement Workflow

**Goal:** Place prepared assets into maps through existing editor actor tools and save evidence.

**Files:**
- Modify: `Python/tools/asset_workflow_tools.py`
- Modify: `Python/tests/test_asset_workflow_tools.py`
- Modify: `Docs/Tools/actor_tools.md`
- Modify: `Docs/Workflows/interactive-asset-workflow.md`

**Tools:**

- `asset_place_in_level_plan`
- `asset_place_in_level`
- `asset_validate_level_placements`

**Implementation Steps:**

- [ ] Reuse existing actor spawn and transform tools rather than duplicating actor placement logic.
- [ ] Require current map evidence from `get_editor_status`.
- [ ] Dry-run returns actor names, classes, transforms, and target map.
- [ ] Mutation returns created actors, changed packages, level snapshot evidence, and save status.
- [ ] Run validation with the sample project and one disposable map if available.

## Slice 9: Project-Pack Asset Recipes

**Goal:** Let consuming projects define asset import expectations and validation gates without hard-coding project specifics in UEMCP.

**Files:**
- Modify: `Docs/Workflows/phase-5-project-pack-boundary.md`
- Modify: `Docs/Workflows/interactive-asset-workflow.md`
- Modify: `Python/tests/test_observability_contract.py`
- Create: `Docs/Workflows/project-asset-recipes.example.json`

**Recipe Shape:**

```json
{
  "asset_recipes": [
    {
      "name": "environment_prototype_pack",
      "target_roots": ["/Game/Environment/Prototype"],
      "allowed_classes": ["StaticMesh", "MaterialInstanceConstant", "Texture2D"],
      "naming_prefixes": ["SM_", "MI_", "T_"],
      "post_import_gates": [
        {"kind": "asset_recipe_classes"},
        {"kind": "asset_recipe_naming"}
      ]
    }
  ]
}
```

**Implementation Steps:**

- [ ] Add docs and tests for recipe shape.
- [ ] Teach compatibility gates to load recipe gates from profile JSON.
- [ ] Ensure neutrality audit permits only generic example paths.
- [ ] Run validation:

```bash
uv --directory Python run pytest tests/test_observability_contract.py
uv --directory Python run python -m uemcp_neutrality
```

## Slice 10: End-to-End Asset Workflow Smoke

**Goal:** Prove the full lane without depending on Fab auth by using a disposable disk-import fixture, while keeping Fab UI flow documented separately.

**Files:**
- Create: `Scripts/Smoke-UEMCPAssetWorkflow.ps1`
- Create: `Python/tests/fixtures/assets/README.md`
- Modify: `Python/tests/test_asset_workflow.py`
- Modify: `Docs/Workflows/interactive-asset-workflow.md`

**Implementation Steps:**

- [ ] Create a smoke script that starts the interactive workflow against a disposable project or explicit user-provided project.
- [ ] Use disk-import fixture files for automation, not Fab marketplace content.
- [ ] Run snapshot before import, disk import, snapshot after import, diff, quality gates, optional cleanup.
- [ ] Keep cleanup editor-backed.
- [ ] Run validation:

```bash
uv --directory Python run pytest
pwsh -NoProfile -File Scripts/Smoke-UEMCPAssetWorkflow.ps1 -SkipFab -CloseLaunchedEditor
```

## Release Gates Per Slice

Every slice must finish with:

```bash
uv --directory Python run pytest
uv --directory Python run python -m uemcp_neutrality
git diff --check
```

Every slice that touches Unreal C++ must also finish with:

```bash
'/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUBT.sh' MCPGameProjectEditor Mac Development -Project="$PWD/MCPGameProject/MCPGameProject.uproject" -Progress -NoEngineChanges -NoHotReloadFromIDE
```

Every slice that changes public tool contracts must update:

- `Scripts/UEMCPToolSurface.Audit.json`
- `Docs/Tools/*.md`
- Python schema/contract tests

Every slice that changes project-specific behavior must keep the behavior in a consuming project pack or ignored local profile, not in public UEMCP defaults.
