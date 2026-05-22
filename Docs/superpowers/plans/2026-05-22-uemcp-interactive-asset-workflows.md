# UEMCP Interactive Asset Workflows Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a separate interactive asset lane so agents can help acquire, import, inspect, organize, place, and validate Unreal assets without making headless smoke depend on Fab.

**Architecture:** Keep the deterministic smoke harness headless and Fab-independent. Add explicit interactive launch/preflight scripts, then grow editor-backed asset intake tools that report structured before/after evidence and keep consuming-project recipes in project packs.

**Tech Stack:** PowerShell launcher scripts, Python MCP tool schemas, Unreal Editor C++ bridge commands, Asset Registry, EditorAssetLibrary, AssetTools, project-owned `Tools/UEMCP` packs.

---

## File Structure

- `Scripts/Start-UEMCPInteractiveAssetWorkflow.ps1`: interactive editor launcher and Fab/project preflight.
- `Docs/Workflows/interactive-asset-workflow.md`: operator and agent workflow contract.
- `Docs/README.md`: links the new workflow.
- `Python/tests/test_asset_workflow.py`: regression tests that keep the interactive lane separate from headless smoke.
- Future `Python/tools/asset_workflow_tools.py`: MCP schemas for asset snapshots, diffs, and imports.
- Future `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPAssetWorkflowCommands.cpp`: editor-backed implementations.
- Future `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/Commands/UnrealMCPAssetWorkflowCommands.h`: command handler interface.

## Task 1: Interactive Workflow Boundary

**Files:**
- Create: `Scripts/Start-UEMCPInteractiveAssetWorkflow.ps1`
- Create: `Docs/Workflows/interactive-asset-workflow.md`
- Modify: `Docs/README.md`
- Test: `Python/tests/test_asset_workflow.py`

- [x] **Step 1: Add tests for the boundary**

Run: `uv --directory Python run pytest tests/test_asset_workflow.py -q`

Expected before implementation: failure because the test file or script is missing.

- [x] **Step 2: Add an interactive launcher**

The launcher must require a consuming project by default, verify Fab is installed and not explicitly disabled unless `-NoFabRequirement` is passed, launch normal editor UI flags only, and optionally wait for UEMCP bridge/editor readiness.

- [x] **Step 3: Add workflow docs**

Document the separation between smoke and interactive asset work, the Fab/auth boundary, the asset-agent ladder, and stop rules.

- [x] **Step 4: Verify**

Run:

```bash
uv --directory Python run pytest tests/test_asset_workflow.py tests/test_mac_readiness.py
uv --directory Python run python -m uemcp_neutrality
git diff --check
```

Expected: tests pass, neutrality passes, diff check is clean.

## Task 2: Asset Intake Snapshots

**Files:**
- Create: `Python/tools/asset_workflow_tools.py`
- Modify: `Python/unreal_mcp_server.py`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/UnrealMCPBridge.cpp`
- Create: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPAssetWorkflowCommands.cpp`
- Create: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/Commands/UnrealMCPAssetWorkflowCommands.h`
- Test: `Python/tests/test_asset_workflow_tools.py`

- [ ] **Step 1: Add failing Python schema tests**

Test that `asset_intake_snapshot` accepts `roots`, `classes`, `include_dependencies`, and `limit`, with `roots` required and no `ctx` property exposed.

- [ ] **Step 2: Add failing bridge contract tests**

Test that `UnrealMCPBridge.cpp` routes `asset_intake_snapshot` to `AssetWorkflowCommands` and that the command source uses `IAssetRegistry`.

- [ ] **Step 3: Implement Python tool builder**

Return a normalized envelope with `snapshot_id`, `roots`, `assets`, `asset_count`, `truncated`, and `warnings`.

- [ ] **Step 4: Implement C++ snapshot command**

Use Asset Registry queries only. Do not load packages unless a later task explicitly requires it.

- [ ] **Step 5: Verify**

Run:

```bash
uv --directory Python run pytest tests/test_asset_workflow_tools.py tests/test_mcp_tool_contract.py
'/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUBT.sh' MCPGameProjectEditor Mac Development -Project="$PWD/MCPGameProject/MCPGameProject.uproject" -Progress -NoEngineChanges -NoHotReloadFromIDE
```

Expected: Python tests pass and UE build succeeds.

## Task 3: Asset Intake Diff

**Files:**
- Modify: `Python/tools/asset_workflow_tools.py`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPAssetWorkflowCommands.cpp`
- Test: `Python/tests/test_asset_workflow_tools.py`

- [ ] **Step 1: Add failing diff tests**

Test `asset_intake_diff` with two snapshot payloads and assert it reports `added`, `removed`, `changed_classes`, and `dependency_changes`.

- [ ] **Step 2: Implement Python validation**

Validate that both inputs are snapshot-shaped objects with asset package names.

- [ ] **Step 3: Implement diff logic**

Compare by package name and object path. Treat class and dependency-list changes as changed assets.

- [ ] **Step 4: Verify**

Run:

```bash
uv --directory Python run pytest tests/test_asset_workflow_tools.py
```

Expected: diff tests pass.

## Task 4: Editor-Backed Disk Import

**Files:**
- Modify: `Python/tools/asset_workflow_tools.py`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPAssetWorkflowCommands.cpp`
- Test: `Python/tests/test_asset_workflow_tools.py`

- [ ] **Step 1: Add failing schema tests**

Test `asset_import_from_disk` requires `source_files` and `destination_path`, supports `replace_existing`, and exposes a `dry_run` flag.

- [ ] **Step 2: Implement dry-run validation**

Return planned imports without touching assets when `dry_run` is true.

- [ ] **Step 3: Implement editor import**

Use Unreal Editor import tasks, destination package paths, and post-import Asset Registry evidence.

- [ ] **Step 4: Verify**

Run Python tests, UE build, and a manual editor import against a disposable project-owned test folder.

## Task 5: Project-Pack Asset Recipes

**Files:**
- Modify: `Docs/Workflows/phase-5-project-pack-boundary.md`
- Modify: `Docs/Workflows/interactive-asset-workflow.md`
- Test: `Python/tests/test_observability_contract.py`

- [ ] **Step 1: Add recipe schema tests**

Test profile-owned asset recipes with `target_roots`, `allowed_classes`, `naming_prefixes`, and `post_import_gates`.

- [ ] **Step 2: Document recipe ownership**

Project-specific asset intent belongs in consuming project packs under `Tools/UEMCP`, not in public UEMCP code.

- [ ] **Step 3: Verify**

Run:

```bash
uv --directory Python run pytest tests/test_observability_contract.py
uv --directory Python run python -m uemcp_neutrality
```

Expected: tests pass and neutrality remains clean.
