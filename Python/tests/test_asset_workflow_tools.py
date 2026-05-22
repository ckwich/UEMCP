import asyncio

from unreal_mcp_server import mcp


class FakeUnrealConnection:
    def __init__(self, response):
        self.response = response
        self.calls = []

    def send_command(self, command, params=None):
        self.calls.append((command, params or {}))
        return self.response


def _tool_schema(tool_name: str):
    async def run_check():
        return await mcp.list_tools()

    tools = asyncio.run(run_check())
    for tool in tools:
        if tool.name == tool_name:
            return tool.inputSchema

    raise AssertionError(f"Tool not found: {tool_name}")


def test_asset_intake_snapshot_schema_is_registered_without_context():
    schema = _tool_schema("asset_intake_snapshot")

    properties = schema.get("properties") or {}
    assert {
        "roots",
        "classes",
        "include_dependencies",
        "include_referencers",
        "include_tags",
        "limit",
    }.issubset(set(properties))
    assert schema.get("required") == ["roots"]
    assert "ctx" not in properties


def test_asset_intake_diff_and_manifest_tools_are_registered_without_context():
    diff_schema = _tool_schema("asset_intake_diff")
    manifest_schema = _tool_schema("asset_intake_write_manifest")

    assert set(diff_schema.get("required") or []) == {"before", "after"}
    assert set(manifest_schema.get("required") or []) == {"diff", "output_path"}
    assert "ctx" not in (diff_schema.get("properties") or {})
    assert "ctx" not in (manifest_schema.get("properties") or {})


def test_build_asset_intake_snapshot_passes_bounded_query_to_bridge():
    from tools.asset_workflow_tools import build_asset_intake_snapshot

    connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "snapshot_id": "asset-snapshot-test",
                "roots": ["/Game/Fab"],
                "filters": {"limit": 10000},
                "asset_count": 0,
                "truncated": False,
                "asset_registry_loading": False,
                "assets": [],
                "warnings": [],
            },
        }
    )

    envelope = build_asset_intake_snapshot(
        lambda: connection,
        roots=["/Game/Fab", "  ", "/Game/Environment"],
        classes=["StaticMesh", "", "Texture2D"],
        include_dependencies=True,
        include_referencers=False,
        include_tags=True,
        limit=20000,
    )

    assert envelope["ok"] is True
    assert envelope["tool"] == "asset_intake_snapshot"
    assert connection.calls == [
        (
            "asset_intake_snapshot",
            {
                "roots": ["/Game/Fab", "/Game/Environment"],
                "classes": ["StaticMesh", "Texture2D"],
                "include_dependencies": True,
                "include_referencers": False,
                "include_tags": True,
                "limit": 10000,
            },
        )
    ]


def test_build_asset_intake_snapshot_refuses_empty_roots_before_bridge_call():
    from tools.asset_workflow_tools import build_asset_intake_snapshot

    connection = FakeUnrealConnection({"status": "success", "result": {}})

    envelope = build_asset_intake_snapshot(lambda: connection, roots=["", "  "])

    assert envelope["ok"] is False
    assert envelope["error"]["category"] == "invalid_asset_intake_snapshot_roots"
    assert connection.calls == []
