"""Tool-surface audit helpers for UEMCP."""

from __future__ import annotations

import ast
import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST_PATH = REPO_ROOT / "Scripts" / "UEMCPToolSurface.Audit.json"
TOOL_MODULE_DIR = Path(__file__).resolve().parent / "tools"
BRIDGE_PATH = (
    REPO_ROOT
    / "MCPGameProject"
    / "Plugins"
    / "UnrealMCP"
    / "Source"
    / "UnrealMCP"
    / "Private"
    / "UnrealMCPBridge.cpp"
)
BRIDGE_COMMAND_PATTERN = re.compile(r'CommandType\s*==\s*TEXT\("([^"]+)"\)')


def _decorator_is_mcp_tool(decorator: ast.expr) -> bool:
    target = decorator
    if isinstance(target, ast.Call):
        target = target.func
    return (
        isinstance(target, ast.Attribute)
        and target.attr == "tool"
        and isinstance(target.value, ast.Name)
        and target.value.id == "mcp"
    )


def registered_mcp_tool_names(tool_module_dir: Path = TOOL_MODULE_DIR) -> list[str]:
    """Return MCP tool names registered from the Python tool modules."""
    names: set[str] = set()
    for path in sorted(tool_module_dir.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        for node in ast.walk(tree):
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            if any(_decorator_is_mcp_tool(decorator) for decorator in node.decorator_list):
                names.add(node.name)
    return sorted(names)


def bridge_command_names(bridge_path: Path = BRIDGE_PATH) -> list[str]:
    """Return command strings routed by the Unreal bridge."""
    text = bridge_path.read_text(encoding="utf-8")
    return sorted(set(BRIDGE_COMMAND_PATTERN.findall(text)))


def load_tool_surface_manifest(path: Path = DEFAULT_MANIFEST_PATH) -> dict[str, Any]:
    """Load the audit manifest and add derived inventory lists."""
    with path.open("r", encoding="utf-8") as manifest_file:
        manifest: dict[str, Any] = json.load(manifest_file)

    tools = manifest.get("tools")
    if not isinstance(tools, dict):
        raise ValueError("tool surface manifest must define a tools object")

    manifest["mcp_tools"] = sorted(
        name
        for name, entry in tools.items()
        if isinstance(entry, dict) and entry.get("exposure") == "mcp_registered"
    )
    manifest["bridge_commands"] = sorted(
        name
        for name, entry in tools.items()
        if isinstance(entry, dict) and entry.get("bridge_command") is True
    )
    return manifest


def audit_tool_surface(manifest: dict[str, Any] | None = None) -> dict[str, list[str]]:
    """Compare the static manifest with the live Python and Unreal bridge surface."""
    manifest = manifest or load_tool_surface_manifest()
    expected_mcp_tools = set(manifest["mcp_tools"])
    actual_mcp_tools = set(registered_mcp_tool_names())
    expected_bridge_commands = set(manifest["bridge_commands"])
    actual_bridge_commands = set(bridge_command_names())

    return {
        "missing_mcp_tools": sorted(actual_mcp_tools - expected_mcp_tools),
        "extra_mcp_tools": sorted(expected_mcp_tools - actual_mcp_tools),
        "missing_bridge_commands": sorted(actual_bridge_commands - expected_bridge_commands),
        "extra_bridge_commands": sorted(expected_bridge_commands - actual_bridge_commands),
    }


def audit_passed(result: dict[str, list[str]]) -> bool:
    return all(not values for values in result.values())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        default=str(DEFAULT_MANIFEST_PATH),
        help="Tool-surface audit manifest JSON.",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print the audit result JSON even when the audit passes.",
    )
    args = parser.parse_args(argv)

    manifest = load_tool_surface_manifest(Path(args.manifest))
    result = audit_tool_surface(manifest)
    if args.json or not audit_passed(result):
        print(json.dumps(result, indent=2, sort_keys=True))
    if not audit_passed(result):
        return 1

    if not args.json:
        print("TOOL_SURFACE_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
