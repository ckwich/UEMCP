import asyncio
import json

import pytest
from mcp.server.mcpserver import MCPServer

from tools.asset_workflow_tools import register_asset_workflow_tools
from tools.blueprint_tools import register_blueprint_tools
from tools.editor_tools import register_editor_tools
from tools.level_workflow_tools import register_level_workflow_tools
from tools.node_tools import register_blueprint_node_tools
from tools.observability_tools import register_observability_tools
from tools.project_tools import register_project_tools
from tools.umg_tools import register_umg_tools
from uemcp_tool_policy import (
    CATALOG_META_TOOLS,
    annotations_for_safety,
    configure_tool_surface,
    resolve_tool_mode,
)
from uemcp_tool_surface import load_tool_surface_manifest


def _fresh_server() -> MCPServer:
    server = MCPServer("UEMCPPolicyTest")
    for register in (
        register_observability_tools,
        register_asset_workflow_tools,
        register_level_workflow_tools,
        register_editor_tools,
        register_blueprint_tools,
        register_blueprint_node_tools,
        register_project_tools,
        register_umg_tools,
    ):
        register(server)
    return server


def _list(server):
    return asyncio.run(server.list_tools())


def _call(server, name, args):
    return asyncio.run(server.call_tool(name, args))


def _call_json(server, name, args):
    result = _call(server, name, args)
    assert not result.is_error, result.content
    return json.loads(result.content[0].text)


def test_default_server_annotates_every_tool_from_manifest():
    from unreal_mcp_server import mcp

    manifest = load_tool_surface_manifest()
    tools = _list(mcp)
    assert tools, "expected registered tools"
    for tool in tools:
        assert tool.annotations is not None, tool.name
        safety = manifest["tools"][tool.name]["safety"]
        assert tool.annotations.read_only_hint is (safety == "read_only"), tool.name


def test_mutation_classes_are_destructive_and_reads_are_not():
    assert annotations_for_safety("read_only").destructive_hint is False
    assert annotations_for_safety("read_only").read_only_hint is True
    for safety in (
        "level_mutation",
        "asset_mutation",
        "blueprint_graph_mutation",
        "project_config_mutation",
    ):
        annotations = annotations_for_safety(safety)
        assert annotations.read_only_hint is False
        assert annotations.destructive_hint is True
    for safety in ("automation_execution", "viewport_or_file_output"):
        annotations = annotations_for_safety(safety)
        assert annotations.read_only_hint is False
        assert annotations.destructive_hint is False


def test_unknown_safety_class_fails_closed():
    annotations = annotations_for_safety("something_new")
    assert annotations.read_only_hint is False
    assert annotations.destructive_hint is True


def test_tool_mode_parsing():
    assert resolve_tool_mode("") == "full"
    assert resolve_tool_mode("FULL") == "full"
    assert resolve_tool_mode(" catalog ") == "catalog"
    with pytest.raises(ValueError):
        resolve_tool_mode("search")


def test_catalog_mode_exposes_only_meta_tools_with_split_permissions():
    server = _fresh_server()
    assert configure_tool_surface(server, "catalog") == "catalog"

    tools = {tool.name: tool for tool in _list(server)}
    assert sorted(tools) == sorted(CATALOG_META_TOOLS)
    assert tools["call_read_tool"].annotations.read_only_hint is True
    assert tools["call_mutating_tool"].annotations.destructive_hint is True
    for tool in tools.values():
        assert "ctx" not in (tool.input_schema.get("properties") or {})


def test_catalog_lists_every_registered_tool_exactly_once():
    full = _fresh_server()
    expected = {tool.name for tool in _list(full)}

    server = _fresh_server()
    configure_tool_surface(server, "catalog")
    listing = _call_json(server, "list_toolsets", {})
    seen = []
    for toolset in listing["toolsets"]:
        described = _call_json(
            server, "describe_toolset", {"toolset": toolset["toolset"]}
        )
        seen.extend(tool["name"] for tool in described["tools"])
    assert sorted(seen) == sorted(expected)
    assert len(seen) == len(set(seen))
    assert "ungrouped" not in {t["toolset"] for t in listing["toolsets"]}


def test_catalog_read_dispatch_runs_python_only_read_tool():
    server = _fresh_server()
    configure_tool_surface(server, "catalog")
    result = _call(
        server,
        "call_read_tool",
        {"tool_name": "get_project_context", "arguments": {"profile_name": "failstate"}},
    )
    assert not result.is_error
    assert result.content[0].text


def test_catalog_refuses_mutation_through_read_dispatcher():
    server = _fresh_server()
    configure_tool_surface(server, "catalog")
    result = _call_json(
        server,
        "call_read_tool",
        {"tool_name": "delete_actor", "arguments": {"name": "Anything"}},
    )
    assert result["status"] == "error"
    assert "call_mutating_tool" in result["error"]


def test_catalog_refuses_read_tool_through_mutating_dispatcher():
    server = _fresh_server()
    configure_tool_surface(server, "catalog")
    result = _call_json(
        server,
        "call_mutating_tool",
        {"tool_name": "uemcp_ping", "arguments": {}},
    )
    assert result["status"] == "error"
    assert "call_read_tool" in result["error"]


def test_catalog_unknown_tool_and_toolset_report_errors():
    server = _fresh_server()
    configure_tool_surface(server, "catalog")
    unknown_tool = _call_json(
        server, "call_read_tool", {"tool_name": "nope", "arguments": {}}
    )
    assert unknown_tool["status"] == "error"
    unknown_set = _call_json(server, "describe_toolset", {"toolset": "nope"})
    assert unknown_set["status"] == "error"
    assert unknown_set["available_toolsets"]
