import asyncio
from pathlib import Path

from unreal_mcp_server import mcp


def _tool_schema(tool_name: str):
    async def run_check():
        return await mcp.list_tools()

    tools = asyncio.run(run_check())
    for tool in tools:
        if tool.name == tool_name:
            return tool.inputSchema

    raise AssertionError(f"Tool not found: {tool_name}")


def test_registered_tool_schemas_do_not_expose_context_parameter():
    async def run_check():
        return await mcp.list_tools()

    tools = asyncio.run(run_check())

    tools_with_context = [
        tool.name
        for tool in tools
        if "ctx" in (tool.inputSchema.get("properties") or {})
        or "ctx" in (tool.inputSchema.get("required") or [])
    ]

    assert tools_with_context == []


def test_python_only_tool_can_be_called_without_context_argument():
    async def run_call():
        return await mcp.call_tool("get_project_context", {"profile_name": "failstate"})

    result = asyncio.run(run_call())

    assert result[0].text


def test_spawn_blueprint_actor_exposes_scale_for_level_replacement_workflows():
    schema = _tool_schema("spawn_blueprint_actor")

    properties = schema.get("properties") or {}
    assert "scale" in properties
    assert "scale" not in (schema.get("required") or [])


def test_spawn_blueprint_actor_bridge_accepts_long_package_paths():
    repo_root = Path(__file__).resolve().parents[2]
    source = (
        repo_root
        / "MCPGameProject"
        / "Plugins"
        / "UnrealMCP"
        / "Source"
        / "UnrealMCP"
        / "Private"
        / "Commands"
        / "UnrealMCPEditorCommands.cpp"
    ).read_text(encoding="utf-8")

    assert 'BlueprintReference.StartsWith(TEXT("/"))' in source
    assert "must reside under /Game/Blueprints" not in source


def test_save_current_level_is_exposed_for_persistent_editor_workflows():
    schema = _tool_schema("save_current_level")

    properties = schema.get("properties") or {}
    assert set(properties) == {"only_if_dirty"}
    assert "only_if_dirty" not in (schema.get("required") or [])


def test_save_current_level_bridge_uses_editor_save_api():
    repo_root = Path(__file__).resolve().parents[2]
    bridge_source = (
        repo_root
        / "MCPGameProject"
        / "Plugins"
        / "UnrealMCP"
        / "Source"
        / "UnrealMCP"
        / "Private"
        / "UnrealMCPBridge.cpp"
    ).read_text(encoding="utf-8")
    editor_source = (
        repo_root
        / "MCPGameProject"
        / "Plugins"
        / "UnrealMCP"
        / "Source"
        / "UnrealMCP"
        / "Private"
        / "Commands"
        / "UnrealMCPEditorCommands.cpp"
    ).read_text(encoding="utf-8")

    assert 'CommandType == TEXT("save_current_level")' in bridge_source
    assert "FEditorFileUtils::SaveLevel" in editor_source
