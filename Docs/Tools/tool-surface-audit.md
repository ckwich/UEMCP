# Tool Surface Audit

UEMCP exposes both Python MCP tools and lower-level Unreal bridge commands. The audit in `Scripts/UEMCPToolSurface.Audit.json` classifies every exposed name so agents can tell read-only observability apart from editor mutation.

Run the gate from `Python`:

```powershell
uv run python -m uemcp_tool_surface
```

Expected passing output:

```text
TOOL_SURFACE_OK
```

The audit compares three surfaces:

- Python MCP tools registered through `@mcp.tool()`.
- Unreal bridge command strings routed by `UnrealMCPBridge.cpp`.
- The safety manifest in `Scripts/UEMCPToolSurface.Audit.json`.

## Safety Classes

- `read_only`: bounded inspection or local history reads. These are the default tools for investigation.
- `automation_execution`: automation or compatibility gates. Use only after readiness is proven and the user intends to run tests.
- `level_mutation`: creates, deletes, moves, or edits actors in the loaded level.
- `asset_mutation`: creates or edits Blueprint or UMG assets.
- `blueprint_graph_mutation`: changes Blueprint graph nodes, variables, or links.
- `project_config_mutation`: changes project settings such as input mappings.
- `viewport_or_file_output`: changes viewport state or writes screenshot/UI preview output.
- `bridge_only`: routed by the Unreal bridge but not exposed as a Python MCP tool.

## MCP Annotations

`Python/uemcp_tool_policy.py` turns each tool's safety class into MCP `ToolAnnotations` when the server starts:

- `read_only` maps to `readOnlyHint: true`, `destructiveHint: false`, and `idempotentHint: true`.
- `automation_execution` and `viewport_or_file_output` map to `readOnlyHint: false` and `destructiveHint: false`.
- Every mutation class, and any unclassified tool, maps to `readOnlyHint: false` and `destructiveHint: true`. This fails closed.

When you change a tool's safety class in the manifest, its advertised annotations change with it.

## Catalog Mode

Set `UEMCP_TOOL_MODE=catalog` to expose four meta-tools instead of the full list: `list_toolsets`, `describe_toolset`, `call_read_tool`, and `call_mutating_tool`. Toolsets come from `tool_groups`, and a tool belongs to the first group that lists it. `call_read_tool` only runs `read_only` tools, and `call_mutating_tool` only runs the rest. See [Official Unreal MCP](../Workflows/official-unreal-mcp.md) for why dispatch is split.

## Agent Rule

Start every Unreal task with the read-only ladder: `uemcp_ping`, `get_editor_status`, `get_editor_readiness`, and, when useful, `diagnose_editor_automation_readiness`. Mutation tools require explicit user intent, a confirmed loaded map or asset, and a follow-up read-only check that proves the result.
