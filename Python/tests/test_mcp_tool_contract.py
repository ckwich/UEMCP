import asyncio

from unreal_mcp_server import mcp


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
