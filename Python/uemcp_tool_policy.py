"""Tool safety annotations and the optional catalog (tool search) mode.

Two ideas adapted from Epic's official Unreal MCP server (UE 5.8):

1. **Annotations.** Every registered tool advertises MCP ``ToolAnnotations``
   derived from the tracked safety manifest
   (``Scripts/UEMCPToolSurface.Audit.json``). Clients that honour
   ``readOnlyHint`` / ``destructiveHint`` can auto-approve observation and
   keep prompting for mutation.

2. **Catalog mode.** Epic's "Tool Search" mode replaces the full tool list
   with ``list_toolsets`` / ``describe_toolset`` / ``call_tool`` to save
   context. A single ``call_tool`` collapses every operation behind one
   permission, so client allowlists stop meaning anything. UEMCP keeps the
   context savings but splits dispatch into ``call_read_tool`` (read-only
   tools only) and ``call_mutating_tool`` (everything else), so approval
   rules still separate looking from touching.

Catalog mode is opt-in: set ``UEMCP_TOOL_MODE=catalog``. The default
(``full``) exposes every tool directly, exactly as before.
"""

from __future__ import annotations

import os
from typing import Any

from mcp.server.mcpserver import Context, MCPServer
from mcp.types import ToolAnnotations

from uemcp_tool_surface import DEFAULT_MANIFEST_PATH, load_tool_surface_manifest


TOOL_MODE_ENV = "UEMCP_TOOL_MODE"
TOOL_MODE_FULL = "full"
TOOL_MODE_CATALOG = "catalog"
CATALOG_META_TOOLS = (
    "list_toolsets",
    "describe_toolset",
    "call_read_tool",
    "call_mutating_tool",
)

READ_ONLY_SAFETY = "read_only"
# Safety classes that change persistent editor, asset, or project state.
DESTRUCTIVE_SAFETY = frozenset(
    {
        "level_mutation",
        "asset_mutation",
        "blueprint_graph_mutation",
        "project_config_mutation",
    }
)
# Not read-only, but do not overwrite existing project content.
ADDITIVE_SAFETY = frozenset({"automation_execution", "viewport_or_file_output"})


def annotations_for_safety(safety: str, title: str | None = None) -> ToolAnnotations:
    """Map a manifest safety class to MCP tool annotations.

    Unknown classes are treated as destructive: an unclassified tool should
    never be auto-approved because of a missing manifest entry.
    """
    if safety == READ_ONLY_SAFETY:
        return ToolAnnotations(
            title=title,
            read_only_hint=True,
            destructive_hint=False,
            idempotent_hint=True,
            open_world_hint=False,
        )
    if safety in ADDITIVE_SAFETY:
        return ToolAnnotations(
            title=title,
            read_only_hint=False,
            destructive_hint=False,
            open_world_hint=False,
        )
    return ToolAnnotations(
        title=title,
        read_only_hint=False,
        destructive_hint=True,
        open_world_hint=False,
    )


def _title_for(tool_name: str) -> str:
    return tool_name.replace("_", " ").strip().capitalize()


def _manifest_tools(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        name: entry
        for name, entry in manifest.get("tools", {}).items()
        if isinstance(entry, dict)
    }


def apply_tool_annotations(
    mcp: MCPServer, manifest: dict[str, Any] | None = None
) -> dict[str, str]:
    """Attach manifest-derived annotations to every registered tool.

    Returns ``{tool_name: safety_class}`` for the tools that were annotated.
    Tools missing from the manifest are annotated as destructive and reported
    under the safety class ``"unclassified"``.
    """
    manifest = manifest or load_tool_surface_manifest(DEFAULT_MANIFEST_PATH)
    entries = _manifest_tools(manifest)
    applied: dict[str, str] = {}
    for tool in mcp._tool_manager.list_tools():
        safety = entries.get(tool.name, {}).get("safety", "unclassified")
        tool.annotations = annotations_for_safety(safety, _title_for(tool.name))
        applied[tool.name] = safety
    return applied


def resolve_tool_mode(value: str | None = None) -> str:
    """Return the configured tool mode, defaulting to full exposure."""
    raw = (value if value is not None else os.environ.get(TOOL_MODE_ENV, "")).strip().lower()
    if raw in ("", TOOL_MODE_FULL):
        return TOOL_MODE_FULL
    if raw == TOOL_MODE_CATALOG:
        return TOOL_MODE_CATALOG
    raise ValueError(
        f"{TOOL_MODE_ENV} must be '{TOOL_MODE_FULL}' or '{TOOL_MODE_CATALOG}', got {raw!r}"
    )


class ToolCatalog:
    """Registered tools grouped by the manifest's ``tool_groups``."""

    def __init__(self, tools: dict[str, Any], manifest: dict[str, Any]):
        self._tools = tools
        self._entries = _manifest_tools(manifest)
        self._groups: dict[str, list[str]] = {}
        grouped: set[str] = set()
        for group, names in manifest.get("tool_groups", {}).items():
            # A tool belongs to the first group that lists it.
            members = [name for name in names if name in tools and name not in grouped]
            if members:
                self._groups[group] = members
                grouped.update(members)
        leftovers = sorted(set(tools) - grouped)
        if leftovers:
            self._groups["ungrouped"] = leftovers

    def safety(self, tool_name: str) -> str:
        return self._entries.get(tool_name, {}).get("safety", "unclassified")

    def is_read_only(self, tool_name: str) -> bool:
        return self.safety(tool_name) == READ_ONLY_SAFETY

    def get(self, tool_name: str) -> Any:
        return self._tools.get(tool_name)

    def list_toolsets(self) -> dict[str, Any]:
        toolsets = []
        for group, names in self._groups.items():
            classes = sorted({self.safety(name) for name in names})
            toolsets.append(
                {
                    "toolset": group,
                    "tool_count": len(names),
                    "read_only": classes == [READ_ONLY_SAFETY],
                    "safety_classes": classes,
                }
            )
        return {
            "status": "ok",
            "mode": TOOL_MODE_CATALOG,
            "toolsets": toolsets,
            "next_step": "Call describe_toolset(toolset) for tool schemas, then "
            "call_read_tool for read-only tools or call_mutating_tool for the rest.",
        }

    def describe_toolset(self, toolset: str) -> dict[str, Any]:
        names = self._groups.get(toolset)
        if names is None:
            return {
                "status": "error",
                "error": f"Unknown toolset: {toolset}",
                "available_toolsets": sorted(self._groups),
            }
        tools = []
        for name in names:
            tool = self._tools[name]
            entry = self._entries.get(name, {})
            tools.append(
                {
                    "name": name,
                    "description": tool.description,
                    "safety": self.safety(name),
                    "dispatch_with": "call_read_tool"
                    if self.is_read_only(name)
                    else "call_mutating_tool",
                    "requires_explicit_user_intent": entry.get(
                        "requires_explicit_user_intent", not self.is_read_only(name)
                    ),
                    "input_schema": tool.parameters,
                }
            )
        return {"status": "ok", "toolset": toolset, "tools": tools}

    async def dispatch(
        self,
        tool_name: str,
        arguments: dict[str, Any] | None,
        context: Context,
        *,
        read_only: bool,
    ) -> Any:
        tool = self._tools.get(tool_name)
        if tool is None:
            return {"status": "error", "error": f"Unknown tool: {tool_name}"}
        if self.is_read_only(tool_name) != read_only:
            expected = "call_read_tool" if self.is_read_only(tool_name) else "call_mutating_tool"
            return {
                "status": "error",
                "error": f"{tool_name} has safety class '{self.safety(tool_name)}'; "
                f"dispatch it with {expected}.",
            }
        return await tool.run(arguments or {}, context, convert_result=False)


def enable_catalog_mode(
    mcp: MCPServer, manifest: dict[str, Any] | None = None
) -> ToolCatalog:
    """Hide the individual tools behind catalog meta-tools."""
    manifest = manifest or load_tool_surface_manifest(DEFAULT_MANIFEST_PATH)
    tools = {tool.name: tool for tool in mcp._tool_manager.list_tools()}
    clashes = sorted(set(CATALOG_META_TOOLS) & set(tools))
    if clashes:
        raise ValueError(f"Catalog meta-tool names already registered: {clashes}")
    catalog = ToolCatalog(tools, manifest)
    for name in tools:
        mcp.remove_tool(name)

    def list_toolsets() -> dict[str, Any]:
        """List UEMCP toolsets with tool counts and safety classes. Start here."""
        return catalog.list_toolsets()

    def describe_toolset(toolset: str) -> dict[str, Any]:
        """Return each tool's description, safety class, dispatcher, and input schema."""
        return catalog.describe_toolset(toolset)

    async def call_read_tool(
        ctx: Context, tool_name: str, arguments: dict[str, Any] | None = None
    ) -> Any:
        """Run a read-only UEMCP tool by name. Refuses tools that mutate state."""
        return await catalog.dispatch(tool_name, arguments, ctx, read_only=True)

    async def call_mutating_tool(
        ctx: Context, tool_name: str, arguments: dict[str, Any] | None = None
    ) -> Any:
        """Run a UEMCP tool that changes editor, asset, or project state.

        Requires explicit user intent and a follow-up read-only check.
        """
        return await catalog.dispatch(tool_name, arguments, ctx, read_only=False)

    mcp.add_tool(list_toolsets, annotations=annotations_for_safety(READ_ONLY_SAFETY, "List toolsets"))
    mcp.add_tool(
        describe_toolset, annotations=annotations_for_safety(READ_ONLY_SAFETY, "Describe toolset")
    )
    mcp.add_tool(
        call_read_tool, annotations=annotations_for_safety(READ_ONLY_SAFETY, "Call read-only tool")
    )
    mcp.add_tool(
        call_mutating_tool,
        annotations=annotations_for_safety("asset_mutation", "Call mutating tool"),
    )
    return catalog


def configure_tool_surface(mcp: MCPServer, mode: str | None = None) -> str:
    """Annotate all tools, then apply the configured exposure mode."""
    manifest = load_tool_surface_manifest(DEFAULT_MANIFEST_PATH)
    apply_tool_annotations(mcp, manifest)
    resolved = resolve_tool_mode(mode)
    if resolved == TOOL_MODE_CATALOG:
        enable_catalog_mode(mcp, manifest)
    return resolved
