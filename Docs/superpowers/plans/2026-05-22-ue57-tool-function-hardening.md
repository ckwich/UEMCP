# UE57 Tool Function Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the UE 5.7 tool-function review defects across the socket bridge, editor actor commands, reflected property reads, Python wrappers, and stale public tool surface.

**Architecture:** Keep UEMCP project-neutral and preserve the existing MCP command names where they are still registered. Harden the transport first, then make editor mutations use editor-world/transaction structure, then bound reflected property output and normalize Python response contracts.

**Tech Stack:** Unreal Engine 5.7 C++ editor plugin, Python FastMCP server, uv/pytest, UEMCP tool-surface and neutrality audits.

---

### Task 1: Lock Regression Gates

**Files:**
- Modify: `Python/tests/test_mcp_tool_contract.py`

- [ ] Add source-level regression checks for socket accumulation/byte sends, value-pointer property writes, editor transactions/subsystem use, bounded actor properties, and stale viewport prompt cleanup.
- [ ] Run `uv --directory Python run --extra dev pytest -q Python/tests/test_mcp_tool_contract.py` and confirm the new checks fail before implementation where applicable.

### Task 2: Harden Socket Protocol Handling

**Files:**
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Public/MCPServerRunnable.h`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/MCPServerRunnable.cpp`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/UnrealMCPBridge.cpp`

- [ ] Replace single-chunk command parsing with accumulated UTF-8 bytes and bounded JSON parsing.
- [ ] Remove stale unused alternate `command`/newline protocol handlers.
- [ ] Send responses with `FTCHARToUTF8` byte length and a loop that handles partial `FSocket::Send` writes.
- [ ] Treat missing or non-object `params` as an empty object, and protect `ExecuteCommand` against GameThread self-deadlock.

### Task 3: Use Editor-Scoped Actor Mutations

**Files:**
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPEditorCommands.cpp`

- [ ] Add helpers for editor availability, editor-world lookup, actor lookup, actor iteration, and dirtying modified level packages.
- [ ] Replace `GWorld` actor queries with the resolved editor world.
- [ ] Wrap spawn/delete/transform/property/Blueprint-spawn mutations in `FScopedTransaction`.
- [ ] Use `UEditorActorSubsystem` for editor actor creation/deletion when available, with safe fallback only where needed.
- [ ] Harden viewport focus null checks and world lookup.

### Task 4: Fix Property Writes And Bound Reflected Properties

**Files:**
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPBlueprintCommands.cpp`
- Modify: `MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPCommonUtils.cpp`
- Modify: `Docs/Tools/actor_tools.md`
- Modify: `Scripts/UEMCPToolSurface.Audit.json`

- [ ] Use `ContainerPtrToValuePtr` for enum and numeric component-property writes.
- [ ] Make reflected actor property output bounded and safe by default: editable/Blueprint-visible public properties, configurable limits, private/transient inclusion only when requested.
- [ ] Update actor tool docs and audit text to describe the bounded default and explicit expansion flags.

### Task 5: Normalize Python Tool Contracts And Stale Surface

**Files:**
- Modify: `Python/tools/editor_tools.py`
- Modify: `Python/unreal_mcp_server.py`
- Modify: `Docs/Tools/editor_tools.md`

- [ ] Stop swallowing transport errors as empty lists/dicts in legacy editor tools.
- [ ] Reduce full command/response logging to bounded summaries.
- [ ] Remove advertised `focus_viewport`/`take_screenshot` MCP prompt entries while leaving bridge-only audit classification explicit.
- [ ] Make the global connection helper honest for per-command reconnect behavior.

### Task 6: Verify And Commit

**Files:**
- All changed files

- [ ] Run `uv --directory Python run --extra dev pytest -q`.
- [ ] Run `uv --directory Python run python -m uemcp_tool_surface`.
- [ ] Run `uv --directory Python run python -m uemcp_neutrality`.
- [ ] Run `git diff --check`.
- [ ] Run UE 5.7 `RunUBT.sh` for `MCPGameProjectEditor Mac Development`.
- [ ] Commit only if the resulting tree is a clean logical boundary and unrelated user changes were not disturbed.
