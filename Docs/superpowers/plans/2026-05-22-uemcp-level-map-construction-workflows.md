# UEMCP Level and Map Construction Workflows Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add dependable editor-owned UEMCP tools for creating, opening, saving, constructing, and validating Unreal levels/maps without shell-editing `.umap` or `.uasset` files.

**Architecture:** Build a dedicated level workflow surface parallel to the completed asset workflow surface. Public UEMCP owns generic level lifecycle tools, declarative batch construction, structured evidence, docs, tests, and smoke coverage; consuming projects own concrete map recipes, sentinel actors, asset choices, and compatibility expectations in project packs.

**Tech Stack:** Python FastMCP tool builders, Unreal Editor C++ bridge commands, UE 5.7 `UEditorLoadingAndSavingUtils`, `FEditorFileUtils`, `UEditorActorSubsystem`, Asset Registry, `UEditorAssetSubsystem`, PowerShell smoke scripts, project-owned JSON recipes.

---

## Steelman

### The strongest version of the requirement

UEMCP should let an agent do the same correct editor-owned map work that currently requires ad hoc Unreal Python: create or open a level, apply a batch construction recipe, save the resulting map packages, validate the resulting actors/assets, and clean up disposable smoke maps. The agent should not need to know whether the consuming project is on macOS or Windows, and it should never mutate Unreal binary assets from the shell.

### The strongest objection to doing this in UEMCP core

Map construction can become game-specific very quickly. A public tool that knows about Failstate rooms, sentinels, combat directors, backpack anchors, or local worktree paths would make UEMCP less reusable and would violate the project-pack boundary. A public arbitrary `run_editor_python` tool would also be powerful but brittle: it is hard to schema-test, hard to audit, easy to abuse, and too easy for agents to drift back into one-off scripts.

### The answer

Expose a narrow generic command surface:

- level lifecycle tools for exact `/Game/...` map package paths.
- a declarative batch construction plan with exact actor names and explicit operations.
- structured preflight and postflight evidence for every mutation.
- project-pack recipes for game-specific content.

The implementation can use UE editor APIs directly in C++ and may use editor-backed Python only for smoke or diagnostic probes if a UE API edge requires it. The public MCP contract should remain explicit typed tools, not an arbitrary Python executor.

### Non-goals for this slice

- No world-partition conversion tool.
- No landscape sculpting, navmesh build, lighting build, data layer authoring, level instance authoring, or streaming-level authoring in the first commit.
- No wildcard deletes, fuzzy actor targeting, or shell-side `.umap` cleanup.
- No Failstate-specific actor names, asset paths, maps, room recipes, compatibility gates, or local directories in UEMCP core.

### Critical risks

- Opening or creating a map while the current map is dirty can lose work. Default behavior must refuse and report dirty packages unless `save_existing=true`.
- World Partition and external actors may dirty actor packages beyond the persistent level package. Save commands must report and save dirty map-related packages, not only the `.umap`.
- Batch construction can quietly create duplicates unless actor identity is explicit. Every operation must use exact `actor_name` or explicit `matching` rules that refuse wildcards.
- `UEditorLoadingAndSavingUtils::LoadMap` expects a filename, while users naturally provide `/Game/...` package paths. The bridge must convert package paths with `FPackageName::LongPackageNameToFilename(..., FPackageName::GetMapPackageExtension())`.
- The existing `asset_place_in_level` tool handles placement in the current level, but level lifecycle and batch construction need their own tool namespace so asset workflows do not become a map editor dumping ground.

### UE 5.7 API evidence

Context7 confirms UE 5.7 editor scripting exposes `ULevelEditorSubsystem` level utility APIs such as `SaveAllDirtyLevels`, `GetCurrentLevel`, and `SetCurrentLevelByName`. Local UE 5.7 source is more precise for this implementation:

- `UEditorLoadingAndSavingUtils::NewBlankMap(bool bSaveExistingMap)` in `/Users/Shared/Epic Games/UE_5.7/Engine/Source/Editor/UnrealEd/Public/FileHelpers.h`.
- `UEditorLoadingAndSavingUtils::NewMapFromTemplate(const FString& PathToTemplateLevel, bool bSaveExistingMap)` in the same header.
- `UEditorLoadingAndSavingUtils::LoadMap(const FString& Filename)` in the same header.
- `UEditorLoadingAndSavingUtils::SaveMap(UWorld* World, const FString& AssetPath)` in the same header.
- `UEditorLoadingAndSavingUtils::SavePackages(const TArray<UPackage*>& PackagesToSave, bool bOnlyDirty)` in the same header.
- `FEditorFileUtils::SaveLevel(ULevel* Level, const FString& Filename)` and `FEditorFileUtils::LoadMap(...)` remain available in `FileHelpers.h` and are already used in UEMCP.
- `UEditorActorSubsystem::SpawnActorFromObject`, `SpawnActorFromClass`, and `DestroyActor` are in `/Users/Shared/Epic Games/UE_5.7/Engine/Source/Editor/UnrealEd/Public/Subsystems/EditorActorSubsystem.h`.

## File Structure

### New files

- `Python/tools/level_workflow_tools.py`
  - Python MCP builders, path validation, dry-run plan validation, and registration for level workflow tools.
- `Python/tests/test_level_workflow_tools.py`
  - Schema, builder, validation, and fake-bridge tests for level lifecycle and construction tools.
- `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/Commands/UnrealMCPLevelWorkflowCommands.h`
  - C++ bridge command class declaration for editor-owned level workflow commands.
- `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPLevelWorkflowCommands.cpp`
  - C++ implementation using UE editor APIs.
- `Docs/Tools/level_workflow_tools.md`
  - Public tool docs and safety guidance.
- `Docs/Workflows/level-map-construction.md`
  - End-to-end workflow guidance for project-pack level recipes.
- `Docs/Workflows/project-map-recipes.example.json`
  - Neutral recipe example for consuming projects.
- `Scripts/Smoke-UEMCPLevelWorkflow.ps1`
  - Disposable editor smoke for create/open/apply/validate/save/cleanup.

### Modified files

- `Python/unreal_mcp_server.py`
  - Register `register_level_workflow_tools(mcp)` after observability and before legacy editor tools.
- `Python/tests/test_mcp_tool_contract.py`
  - Assert bridge routes new level workflow commands to `FUnrealMCPLevelWorkflowCommands`; assert `FTSTicker` dispatch remains.
- `Python/tests/test_tool_surface_audit.py`
  - Existing coverage should catch manifest drift once the audit JSON is updated.
- `Scripts/UEMCPToolSurface.Audit.json`
  - Add level lifecycle and construction tools with `level_mutation` or `read_only` safety.
- `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/UnrealMCPBridge.cpp`
  - Instantiate and route `FUnrealMCPLevelWorkflowCommands`.
- `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/UnrealMCPBridge.h`
  - Add the level workflow command member if the bridge header currently owns command members.
- `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/UnrealMCP.Build.cs`
  - Add missing editor modules only if compile proves they are required; likely current `UnrealEd`, `EditorSubsystem`, `EditorScriptingUtilities`, `AssetRegistry`, and `AssetTools` are enough.
- `Docs/README.md`
  - Link the level/map construction workflow.
- `Docs/Tools/actor_tools.md`
  - Point batch map construction users to level workflow tools instead of legacy actor one-offs.
- `Docs/Tools/editor_tools.md`
  - Clarify `save_current_level` is legacy/lower-level; prefer `level_save` for structured evidence.
- `Python/README.md`
  - Add a short level workflow tool list.

## Public Tool Surface

### `level_list_maps`

Read-only Asset Registry query for map packages.

```json
{
  "roots": ["/Game/Maps"],
  "limit": 500
}
```

Returns `maps` entries with `package_name`, `asset_name`, `object_path`, `package_path`, `is_world_asset`, and `asset_class`.

### `level_create`

Create a new editor map and optionally save it to an exact package path.

```json
{
  "package_path": "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
  "template_path": null,
  "save_existing": false,
  "save_new_level": true,
  "fail_if_exists": true,
  "dry_run": true
}
```

Default behavior refuses dirty current maps unless `save_existing=true`.

### `level_open`

Open an exact map package path through editor APIs.

```json
{
  "package_path": "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
  "save_existing": false,
  "require_exists": true
}
```

The bridge converts the package path to a map filename before calling `UEditorLoadingAndSavingUtils::LoadMap`.

### `level_save`

Save the current map or an exact loaded target map with structured dirty-package evidence.

```json
{
  "package_path": "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
  "only_if_dirty": true,
  "include_external_actor_packages": true
}
```

### `level_construction_plan`

Python-only dry-run validator for batch construction.

```json
{
  "target_map": "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
  "operations": [
    {
      "op": "ensure_actor",
      "actor_name": "UEMCP_LevelSmoke_PointLight",
      "actor_class": "/Script/Engine.PointLight",
      "label": "UEMCP Level Smoke Point Light",
      "folder_path": "UEMCP/Smoke",
      "location": [0.0, 0.0, 180.0],
      "rotation": [0.0, 0.0, 0.0],
      "scale": [1.0, 1.0, 1.0],
      "tags": ["UEMCPSmoke"]
    }
  ]
}
```

Allowed operations in this slice:

- `ensure_actor`
- `set_actor_transform`
- `set_actor_folder`
- `set_actor_label`
- `set_actor_tags`
- `delete_actor`

Every operation targets exact actor names. No wildcard matching.

### `level_apply_construction_plan`

Editor-backed executor for a validated plan.

```json
{
  "target_map": "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
  "open_level": true,
  "create_if_missing": false,
  "save_level": true,
  "dry_run": false,
  "operations": []
}
```

Returns `opened_map`, `created_actors`, `updated_actors`, `deleted_actors`, `changed_packages`, `saved_packages`, `warnings`, and `validation_summary`.

### `level_validate_construction`

Read-only validation of current or target map construction evidence.

```json
{
  "target_map": "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
  "expected_actors": [
    {
      "actor_name": "UEMCP_LevelSmoke_PointLight",
      "class": "PointLight",
      "folder_path": "UEMCP/Smoke",
      "tags": ["UEMCPSmoke"],
      "location": [0.0, 0.0, 180.0]
    }
  ],
  "location_tolerance": 0.1
}
```

## Task 1: Python Level Workflow Tool Builders

**Files:**
- Create: `Python/tools/level_workflow_tools.py`
- Create: `Python/tests/test_level_workflow_tools.py`
- Modify: `Python/unreal_mcp_server.py`

- [ ] **Step 1: Write registration tests**

Add tests that assert these tools are registered without `ctx`:

```python
def test_level_workflow_tools_are_registered_without_context():
    expected_required = {
        "level_list_maps": {"roots"},
        "level_create": {"package_path"},
        "level_open": {"package_path"},
        "level_save": set(),
        "level_construction_plan": {"operations"},
        "level_apply_construction_plan": {"operations"},
        "level_validate_construction": {"expected_actors"},
    }
    for tool_name, required in expected_required.items():
        schema = _tool_schema(tool_name)
        properties = schema.get("properties") or {}
        assert required.issubset(set(schema.get("required") or [])), tool_name
        assert "ctx" not in properties, tool_name
```

- [ ] **Step 2: Write builder validation tests**

Add fake-connection tests that prove:

- `level_create` refuses paths outside `/Game/`.
- `level_open` refuses wildcards.
- `level_save` accepts no package path and forwards `package_path=None`.
- `level_construction_plan` normalizes transforms and refuses missing `actor_name`.
- `level_apply_construction_plan` calls `level_apply_construction_plan` with the normalized operations.
- `level_validate_construction` refuses an empty expected actor list.

- [ ] **Step 3: Run targeted failing tests**

Run:

```bash
uv --directory Python run pytest tests/test_level_workflow_tools.py -q
```

Expected: fail because `Python/tools/level_workflow_tools.py` does not exist and tools are not registered.

- [ ] **Step 4: Implement `Python/tools/level_workflow_tools.py`**

Follow `Python/tools/asset_workflow_tools.py` patterns:

- define `MAX_LEVEL_OPERATION_LIMIT = 1000`.
- reuse `execute_bridge_command`, `build_success_envelope`, `build_error_envelope`, and `utc_now`.
- implement `_clean_string_list`, `_is_game_path`, `_has_wildcard`, `_validate_game_path`, and `_normalize_vector`.
- implement builders for all public tools.
- keep default mutation arguments conservative: `dry_run=True`, `save_existing=False`, `save_level=False`.

- [ ] **Step 5: Register the tools**

Modify `Python/unreal_mcp_server.py`:

```python
from tools.level_workflow_tools import register_level_workflow_tools

register_observability_tools(mcp)
register_asset_workflow_tools(mcp)
register_level_workflow_tools(mcp)
register_editor_tools(mcp)
```

- [ ] **Step 6: Re-run targeted tests**

Run:

```bash
uv --directory Python run pytest tests/test_level_workflow_tools.py -q
```

Expected: pass.

## Task 2: Bridge Command Class and Routing

**Files:**
- Create: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/Commands/UnrealMCPLevelWorkflowCommands.h`
- Create: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPLevelWorkflowCommands.cpp`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/UnrealMCPBridge.cpp`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/UnrealMCPBridge.h`
- Modify: `Python/tests/test_mcp_tool_contract.py`

- [ ] **Step 1: Write bridge route tests**

Add a test that reads `UnrealMCPBridge.cpp` and asserts every new command is routed:

```python
for command_name in [
    "level_list_maps",
    "level_create",
    "level_open",
    "level_save",
    "level_apply_construction_plan",
    "level_validate_construction",
]:
    assert f'CommandType == TEXT("{command_name}")' in bridge_source
assert "LevelWorkflowCommandsForTask->HandleCommand" in bridge_source
```

Add a source test that reads the new `.cpp` once implemented and asserts it contains `UEditorLoadingAndSavingUtils`, `UEditorActorSubsystem`, and `SavePackages`.

- [ ] **Step 2: Run failing contract test**

Run:

```bash
uv --directory Python run pytest tests/test_mcp_tool_contract.py::test_level_workflow_bridge_routes_lifecycle_and_construction_commands -q
```

Expected: fail until the bridge class and routing exist.

- [ ] **Step 3: Add the C++ header**

Declare `FUnrealMCPLevelWorkflowCommands` with:

- `HandleCommand`
- `HandleLevelListMaps`
- `HandleLevelCreate`
- `HandleLevelOpen`
- `HandleLevelSave`
- `HandleLevelApplyConstructionPlan`
- `HandleLevelValidateConstruction`

- [ ] **Step 4: Add bridge member and routing**

Mirror the `AssetWorkflowCommands` wiring:

- include `Commands/UnrealMCPLevelWorkflowCommands.h`.
- allocate `LevelWorkflowCommands`.
- capture `LevelWorkflowCommandsForTask`.
- route the six editor-backed commands to `LevelWorkflowCommandsForTask->HandleCommand`.
- leave `level_construction_plan` Python-only.

- [ ] **Step 5: Re-run contract tests**

Run:

```bash
uv --directory Python run pytest tests/test_mcp_tool_contract.py -q
```

Expected: pass.

## Task 3: Level Lifecycle Implementation

**Files:**
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPLevelWorkflowCommands.cpp`
- Modify: `Python/tests/test_mcp_tool_contract.py`

- [ ] **Step 1: Implement shared C++ helpers**

Add helpers in an anonymous namespace:

- `NormalizePackagePath`
- `IsExactGamePackagePath`
- `ObjectPathFromPackagePath`
- `PackageLeafName`
- `CurrentMapFromWorld`
- `ResolveEditorWorld`
- `MapPackageExists`
- `MapFilenameFromPackagePath`
- `CollectDirtyWorldPackages`
- `JsonArrayFromStringArray`

`MapFilenameFromPackagePath` must use:

```cpp
FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetMapPackageExtension())
```

- [ ] **Step 2: Implement `level_list_maps`**

Use Asset Registry with class path `UWorld` or class-name fallback matching observed UE 5.7 registry data. Return bounded `maps`, `map_count`, `truncated`, `roots`, and `asset_registry_loading`.

- [ ] **Step 3: Implement dirty current map preflight**

Before `level_create` or `level_open`:

- resolve editor world.
- collect dirty map-related packages.
- if dirty and `save_existing=false`, return error JSON with `dirty_packages`.
- if dirty and `save_existing=true`, call `UEditorLoadingAndSavingUtils::SavePackages`.

- [ ] **Step 4: Implement `level_create`**

Behavior:

- validate exact `/Game/...` package path.
- if `fail_if_exists=true` and map exists, return error.
- if `dry_run=true`, return planned operations without creating a map.
- call `UEditorLoadingAndSavingUtils::NewBlankMap(false)` when `template_path` is empty.
- call `UEditorLoadingAndSavingUtils::NewMapFromTemplate(TemplateFilename, false)` when `template_path` is present.
- if `save_new_level=true`, call `UEditorLoadingAndSavingUtils::SaveMap(NewWorld, PackagePath)`.
- return `created`, `package_path`, `current_map`, `saved`, `saved_packages`, and `dirty_packages_after`.

- [ ] **Step 5: Implement `level_open`**

Behavior:

- validate exact `/Game/...` package path.
- require map exists when `require_exists=true`.
- convert package path to filename.
- call `UEditorLoadingAndSavingUtils::LoadMap(Filename)`.
- return `opened`, `package_path`, `current_map_before`, `current_map_after`, and warnings.

- [ ] **Step 6: Implement `level_save`**

Behavior:

- when `package_path` is omitted, save the current editor world.
- when `package_path` is provided, refuse if it does not match the current map in this first slice; do not silently open maps just to save them.
- collect world package and dirty external actor packages when `include_external_actor_packages=true`.
- call `UEditorLoadingAndSavingUtils::SavePackages(Packages, only_if_dirty)`.
- return `saved`, `saved_packages`, `skipped_clean_packages`, `dirty_packages_before`, and `dirty_packages_after`.

- [ ] **Step 7: Compile**

Run:

```bash
'/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUBT.sh' MCPGameProjectEditor Mac Development -Project="$PWD/MCPGameProject/MCPGameProject.uproject" -Progress -NoEngineChanges -NoHotReloadFromIDE
```

Expected: `Result: Succeeded`.

## Task 4: Batch Construction Plan Execution

**Files:**
- Modify: `Python/tools/level_workflow_tools.py`
- Modify: `Python/tests/test_level_workflow_tools.py`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPLevelWorkflowCommands.cpp`

- [ ] **Step 1: Strengthen Python plan normalization**

`level_construction_plan` must normalize each operation into this shape:

```json
{
  "op": "ensure_actor",
  "actor_name": "ExactActorName",
  "actor_class": "/Script/Engine.PointLight",
  "asset_path": null,
  "label": "Exact Actor Label",
  "folder_path": "Folder/Subfolder",
  "location": [0.0, 0.0, 0.0],
  "rotation": [0.0, 0.0, 0.0],
  "scale": [1.0, 1.0, 1.0],
  "tags": []
}
```

Refuse:

- missing `actor_name`.
- wildcard actor names.
- unsupported `op`.
- `ensure_actor` without either `actor_class` or `asset_path`.
- `delete_actor` unless `confirm_delete=true`.

- [ ] **Step 2: Implement actor lookup helpers**

In C++:

- exact actor name lookup by `Actor->GetName()`.
- exact label lookup only for validation, not mutation targeting.
- return ambiguity errors if more than one actor matches an exact label.

- [ ] **Step 3: Implement `ensure_actor`**

Behavior:

- if actor exists by exact name, update transform, label, folder, tags.
- if actor does not exist, spawn it:
  - when `asset_path` is provided, load asset through `UEditorAssetSubsystem::LoadAsset` and call `UEditorActorSubsystem::SpawnActorFromObject`.
  - when `actor_class` is provided, resolve class and call `UEditorActorSubsystem::SpawnActorFromClass`.
- rename created actor to `actor_name` with `Actor->Rename(*ActorName)` only when safe; otherwise use `SetActorLabel` and report the generated object name separately.
- set transform with `SetActorLocation`, `SetActorRotation`, and `SetActorScale3D`.
- set folder with `Actor->SetFolderPath(FName(*FolderPath))`.
- set tags via `Actor->Tags`.
- call `Actor->Modify()` inside `FScopedTransaction`.

- [ ] **Step 4: Implement update/delete operations**

Implement:

- `set_actor_transform`
- `set_actor_folder`
- `set_actor_label`
- `set_actor_tags`
- `delete_actor`

Use `UEditorActorSubsystem::DestroyActor` for deletes and require exact actor names.

- [ ] **Step 5: Implement dry-run evidence**

When `dry_run=true`, do not spawn, update, delete, save, or open levels. Return:

- operation count.
- would-create actor names.
- would-update actor names.
- would-delete actor names.
- missing source assets/classes.
- current map.
- requested target map.

- [ ] **Step 6: Implement save integration**

When `save_level=true`, call the new level-save helper after applying the plan. Include changed packages from:

- editor world package.
- actor packages.
- actor external packages.

- [ ] **Step 7: Compile and run Python tests**

Run:

```bash
uv --directory Python run pytest tests/test_level_workflow_tools.py tests/test_mcp_tool_contract.py -q
'/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUBT.sh' MCPGameProjectEditor Mac Development -Project="$PWD/MCPGameProject/MCPGameProject.uproject" -Progress -NoEngineChanges -NoHotReloadFromIDE
```

Expected: tests pass and UBT succeeds.

## Task 5: Construction Validation

**Files:**
- Modify: `Python/tools/level_workflow_tools.py`
- Modify: `Python/tests/test_level_workflow_tools.py`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPLevelWorkflowCommands.cpp`

- [ ] **Step 1: Add validation builder tests**

Test:

- expected actor names are required.
- optional expected class/folder/tags/location are forwarded.
- location tolerance defaults to `1.0`.
- invalid target maps are rejected before bridge calls.

- [ ] **Step 2: Implement bridge validation**

For each expected actor:

- find by exact actor name.
- compare class short name or class path when provided.
- compare label when provided.
- compare folder path when provided.
- compare required tags.
- compare location within tolerance when provided.
- return `found`, `missing`, `mismatched`, `passed`, and per-actor evidence.

- [ ] **Step 3: Require current map match when target_map is provided**

Validation should not silently open maps. If `target_map` is provided and the current map differs, return a failed validation with `current_map` and `expected_map`.

- [ ] **Step 4: Run targeted tests and compile**

Run:

```bash
uv --directory Python run pytest tests/test_level_workflow_tools.py tests/test_mcp_tool_contract.py -q
'/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUBT.sh' MCPGameProjectEditor Mac Development -Project="$PWD/MCPGameProject/MCPGameProject.uproject" -Progress -NoEngineChanges -NoHotReloadFromIDE
```

Expected: pass.

## Task 6: Tool Surface Audit and Docs

**Files:**
- Modify: `Scripts/UEMCPToolSurface.Audit.json`
- Create: `Docs/Tools/level_workflow_tools.md`
- Create: `Docs/Workflows/level-map-construction.md`
- Create: `Docs/Workflows/project-map-recipes.example.json`
- Modify: `Docs/README.md`
- Modify: `Docs/Tools/actor_tools.md`
- Modify: `Docs/Tools/editor_tools.md`
- Modify: `Python/README.md`

- [ ] **Step 1: Update tool-surface manifest**

Add:

- `level_list_maps`: category `level_read`, safety `read_only`, explicit intent `false`.
- `level_create`: category `level_mutation`, safety `level_mutation`, explicit intent `true`.
- `level_open`: category `level_mutation`, safety `level_mutation`, explicit intent `true`.
- `level_save`: category `level_mutation`, safety `level_mutation`, explicit intent `true`.
- `level_construction_plan`: category `level_planning`, safety `read_only`, explicit intent `false`.
- `level_apply_construction_plan`: category `level_mutation`, safety `level_mutation`, explicit intent `true`.
- `level_validate_construction`: category `level_read`, safety `read_only`, explicit intent `false`.

If `level_planning` is too much manifest churn, classify `level_construction_plan` as `level_read` and document it as Python-only dry-run validation.

- [ ] **Step 2: Write docs**

Docs must state:

- UEMCP never shell-edits `.umap` or `.uasset`.
- `level_create` and `level_open` refuse dirty current maps unless explicit save is requested.
- project packs own concrete recipes.
- batch construction requires exact actor identity.
- `asset_place_in_level` remains useful for simple asset placement; use level workflow tools for map lifecycle and multi-step construction.

- [ ] **Step 3: Add neutral recipe example**

Use only neutral `/Game/UEMCP/...` paths and built-in actor classes:

```json
{
  "name": "uemcp_level_smoke",
  "target_map": "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
  "operations": [
    {
      "op": "ensure_actor",
      "actor_name": "UEMCP_LevelSmoke_PointLight",
      "actor_class": "/Script/Engine.PointLight",
      "label": "UEMCP Level Smoke Point Light",
      "folder_path": "UEMCP/Smoke",
      "location": [0.0, 0.0, 180.0],
      "rotation": [0.0, 0.0, 0.0],
      "scale": [1.0, 1.0, 1.0],
      "tags": ["UEMCPSmoke"]
    }
  ],
  "expected_actors": [
    {
      "actor_name": "UEMCP_LevelSmoke_PointLight",
      "class": "PointLight",
      "folder_path": "UEMCP/Smoke",
      "tags": ["UEMCPSmoke"],
      "location": [0.0, 0.0, 180.0]
    }
  ]
}
```

- [ ] **Step 4: Run docs/surface validation**

Run:

```bash
uv --directory Python run pytest tests/test_tool_surface_audit.py -q
uv --directory Python run python -m uemcp_tool_surface
uv --directory Python run python -m uemcp_neutrality
```

Expected: pass, `TOOL_SURFACE_OK`, `NEUTRALITY_OK`.

## Task 7: Level Workflow Smoke

**Files:**
- Create: `Scripts/Smoke-UEMCPLevelWorkflow.ps1`
- Modify: `Python/tests/test_asset_workflow.py` or create `Python/tests/test_level_workflow_smoke_script.py`

- [ ] **Step 1: Add script fixture test**

Test that the smoke script exists and contains:

- `level_create`
- `level_apply_construction_plan`
- `level_validate_construction`
- `level_save`
- `asset_delete`
- `LEVEL_WORKFLOW_SMOKE_OK`
- `-CloseLaunchedEditor`

- [ ] **Step 2: Implement smoke script**

Use the existing asset workflow smoke script structure:

- build plugin/editor if requested.
- launch harness editor if the bridge is not listening.
- call `level_create` for `/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke`.
- call `level_apply_construction_plan` with a built-in `PointLight`.
- call `level_validate_construction`.
- call `level_save`.
- open a blank unsaved map or the default project map before cleanup.
- cleanup the smoke map through `asset_delete`.
- close the launched editor when `-CloseLaunchedEditor` is supplied.

- [ ] **Step 3: Run script fixture test**

Run:

```bash
uv --directory Python run pytest tests/test_level_workflow_smoke_script.py -q
```

Expected: pass.

- [ ] **Step 4: Run full smoke**

Run:

```bash
pwsh -NoProfile -ExecutionPolicy Bypass -File ./Scripts/Smoke-UEMCPLevelWorkflow.ps1 -CloseLaunchedEditor
```

Expected output includes:

- `LEVEL_WORKFLOW_BRIDGE_READY`
- `LEVEL_WORKFLOW_EDITOR_READY`
- `LEVEL_WORKFLOW_SMOKE_CREATED package=/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke`
- `LEVEL_WORKFLOW_SMOKE_APPLIED created=1`
- `LEVEL_WORKFLOW_SMOKE_VALIDATED passed=true`
- `LEVEL_WORKFLOW_SMOKE_CLEANUP_DELETED package=/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke`
- `LEVEL_WORKFLOW_SMOKE_OK`

## Task 8: Project-Pack Recipe Gates

**Files:**
- Modify: `Python/tools/observability_tools.py`
- Modify: `Python/tests/test_observability_tools.py`
- Modify: `Docs/Workflows/level-map-construction.md`

- [ ] **Step 1: Add tests for `map_recipe` gates**

Add profile compatibility gate tests for:

- `map_recipe_current_map`
- `map_recipe_expected_actors`
- `map_recipe_min_actor_count`

- [ ] **Step 2: Implement read-only recipe gates**

In `observability_tools`, call existing read-only bridge commands:

- `get_editor_status` for current map.
- `get_level_snapshot` for actor evidence.

Do not call mutating level workflow commands from compatibility gates.

- [ ] **Step 3: Document project-pack ownership**

Docs must say concrete recipes live in:

```text
Tools/UEMCP/recipes/maps/*.json
```

or in an ignored local profile folder for machine-specific tests.

- [ ] **Step 4: Validate**

Run:

```bash
uv --directory Python run pytest tests/test_observability_tools.py -q
uv --directory Python run python -m uemcp_neutrality
```

Expected: pass and `NEUTRALITY_OK`.

## Final Validation

Run the complete gate before committing:

```bash
uv --directory Python run pytest -q
uv --directory Python run python -m uemcp_neutrality
uv --directory Python run python -m uemcp_tool_surface
git diff --check
'/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUBT.sh' MCPGameProjectEditor Mac Development -Project="$PWD/MCPGameProject/MCPGameProject.uproject" -Progress -NoEngineChanges -NoHotReloadFromIDE
pwsh -NoProfile -ExecutionPolicy Bypass -File ./Scripts/Smoke-UEMCPLevelWorkflow.ps1 -CloseLaunchedEditor
```

Expected:

- all Python tests pass.
- `NEUTRALITY_OK`.
- `TOOL_SURFACE_OK`.
- UBT `Result: Succeeded`.
- smoke prints `LEVEL_WORKFLOW_SMOKE_OK`.
- no `.umap`, `.uasset`, redirector, or generated smoke content remains under `MCPGameProject/Content` after cleanup.

## Commit Plan

Use small commits if implementing manually:

1. `feat: add level workflow mcp builders`
2. `feat: route level workflow bridge commands`
3. `feat: add editor level lifecycle tools`
4. `feat: apply declarative level construction plans`
5. `feat: validate constructed level content`
6. `docs: document level map construction workflows`
7. `test: add level workflow smoke`
8. `feat: add map recipe readiness gates`

If implemented in one focused session after each task is validated, a single final commit is acceptable:

```bash
git commit -m "feat: add level map construction workflows"
```

## Self-Review

- Spec coverage: the plan covers create/open/save, batch construction, validation, smoke, project-pack recipes, and binary asset safety.
- Placeholder scan: every command name, file path, validation gate, and smoke expectation is named.
- Type consistency: public names use the `level_*` namespace; fields consistently use `package_path`, `target_map`, `operations`, `actor_name`, and array transforms.
- Boundary check: no Failstate-specific public paths, actor names, or recipes are present.
- Risk check: dirty-map handling, exact actor identity, map package path conversion, and external actor package saving are explicit implementation gates.
