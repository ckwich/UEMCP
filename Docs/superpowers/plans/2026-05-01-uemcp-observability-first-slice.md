# UEMCP Observability First Slice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the first read-mostly observability foundation for the UEMCP fork.

**Architecture:** Keep the upstream Python MCP server and Unreal Editor plugin bridge, but add a small Python contract layer for envelopes, error classification, Failstate profile loading, and observability tools. Add minimal Unreal-side read-only command handlers for editor status and bounded log output so the Python tools can return real editor evidence when the plugin is installed.

**Tech Stack:** Python 3.10+, `mcp.server.fastmcp`, `pytest`, Unreal Engine editor plugin C++.

---

## File Map

- Create `Python/uemcp_observability.py`: response envelopes, error classification, profile loading, and bridge command helpers.
- Create `Python/tools/observability_tools.py`: MCP tool registrations for `uemcp_ping`, `get_editor_status`, `get_output_log`, and `get_failstate_context`.
- Create `Python/profiles/failstate.json`: default Failstate target/project profile.
- Create `Python/tests/test_observability_contract.py`: envelope and error classification tests.
- Create `Python/tests/test_observability_tools.py`: Python tool helper tests using fake bridge connections.
- Create `Python/tests/test_failstate_profile.py`: profile loading tests.
- Modify `Python/unreal_mcp_server.py`: register observability tools and expose stable metadata.
- Modify `Python/pyproject.toml`: rebrand package metadata, add pytest dev dependency, include new modules/packages, and constrain `mcp`/`fastmcp` to the checked-in compatible range.
- Modify `mcp.json`: point at this repo's Python directory and use the UEMCP server name.
- Modify `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/UnrealMCPBridge.cpp`: route `get_editor_status` and `get_output_log`.
- Modify `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/Commands/UnrealMCPEditorCommands.h`: declare read-only status/log handlers.
- Modify `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPEditorCommands.cpp`: implement read-only status/log handlers.

## Task 1: Python Contract Tests

- [x] **Step 1: Write failing envelope tests**

Create tests asserting:

- A success envelope contains `ok`, `tool`, `request_id`, `started_at`, `finished_at`, `duration_ms`, `data`, `warnings`, and `error`.
- An error envelope classifies connection failures as `connection_failed`.
- A raw Unreal response with `status: success` unwraps `result`.

Run:

```powershell
uv run pytest tests/test_observability_contract.py -q
```

Expected before implementation: import failure for `uemcp_observability`.

- [x] **Step 2: Implement minimal contract layer**

Create `uemcp_observability.py` with deterministic envelope helpers and error classification.

- [x] **Step 3: Verify contract tests pass**

Run:

```powershell
uv run pytest tests/test_observability_contract.py -q
```

Expected after implementation: all tests pass.

## Task 2: Failstate Profile

- [x] **Step 1: Write failing profile tests**

Create tests asserting:

- `load_profile("failstate")` reads `profiles/failstate.json`.
- The profile includes `project_path`, `preferred_worktree_path`, `engine_version`, `content_roots`, `automation_test_prefixes`, and `log_categories`.
- `get_failstate_context_data()` returns profile data plus warnings instead of touching Unreal.

Run:

```powershell
uv run pytest tests/test_failstate_profile.py -q
```

Expected before implementation: missing profile loader/profile file.

- [x] **Step 2: Add profile file and loader**

Create `Python/profiles/failstate.json` and implement profile helpers in `uemcp_observability.py`.

- [x] **Step 3: Verify profile tests pass**

Run:

```powershell
uv run pytest tests/test_failstate_profile.py -q
```

Expected after implementation: all tests pass.

## Task 3: Observability Tool Helpers

- [x] **Step 1: Write failing tool helper tests**

Create tests using fake Unreal connections asserting:

- `build_uemcp_ping()` returns an ok envelope when bridge `ping` succeeds.
- `build_uemcp_ping()` returns a structured `connection_failed` envelope when no bridge connection exists.
- `build_editor_status()` unwraps editor data from `get_editor_status`.
- `build_output_log()` passes `limit`, `category`, and `verbosity` params to `get_output_log`.

Run:

```powershell
uv run pytest tests/test_observability_tools.py -q
```

Expected before implementation: missing helper functions.

- [x] **Step 2: Implement helper functions and MCP registration**

Add `tools/observability_tools.py` and register it from `unreal_mcp_server.py`.

- [x] **Step 3: Verify tool tests pass**

Run:

```powershell
uv run pytest tests/test_observability_tools.py -q
```

Expected after implementation: all tests pass.

## Task 4: Metadata and MCP Config

- [x] **Step 1: Rebrand Python metadata safely**

Set package name to `uemcp`, update description, add `pytest` optional dev dependency, include `uemcp_observability` in `py-modules`, and constrain current compatible MCP dependencies.

- [x] **Step 2: Update MCP config**

Point `mcp.json` at `C:\Dev\UEMCP\Python` and name the server `uemcp`.

- [x] **Step 3: Verify import and tests**

Run:

```powershell
uv lock --check
uv run python -c "from unreal_mcp_server import mcp; print(type(mcp).__name__)"
uv run pytest -q
```

Expected: lock check may fail until `uv lock` is run after metadata changes; import and tests must pass after lock refresh.

## Task 5: Unreal Read-Only Commands

- [x] **Step 1: Route new commands**

Add `get_editor_status` and `get_output_log` to the editor command routing.

- [x] **Step 2: Implement status payload**

Return plugin version, engine version, project name/path, current map, PIE status, selected actor count, and dirty package count when available.

- [x] **Step 3: Implement bounded log placeholder**

Return a bounded response shape with an explicit warning if historical output log capture is not yet wired. This must be honest and structured, not a fake log.

- [x] **Step 4: Run textual C++ checks**

Run:

```powershell
rg "get_editor_status|get_output_log" MCPGameProject\Plugins\UnrealMCP\Source\UnrealMCP
```

Expected: command routing, declarations, and implementations are present.

## Task 6: Final Verification and Commit

- [x] **Step 1: Run fresh verification**

Run:

```powershell
uv run pytest -q
uv run python -c "from unreal_mcp_server import mcp; print(type(mcp).__name__)"
git diff --check
git status --short --branch
```

- [ ] **Step 2: Commit one clean implementation slice**

Stage only the observability slice files and commit:

```powershell
git commit -m "feat: add observability foundation"
```

- [ ] **Step 3: Record Engram memory**

Write an Engram entry with repo path, branch, commit, files changed, validation, and next recommended step.
