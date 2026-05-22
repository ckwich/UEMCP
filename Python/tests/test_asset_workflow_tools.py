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


def test_asset_workflow_mutation_and_placement_tools_are_registered_without_context():
    expected_required = {
        "asset_import_from_disk": {"source_files", "destination_path"},
        "asset_organize_plan": {"operations"},
        "asset_rename": {"asset_path", "new_name"},
        "asset_move": {"asset_path", "destination_path"},
        "asset_duplicate": {"asset_path", "destination_path"},
        "asset_delete": {"asset_path"},
        "asset_save_packages": {"package_paths"},
        "asset_fixup_redirectors": {"roots"},
        "asset_prepare_for_level": {"asset_path"},
        "asset_create_blueprint_wrapper": {"asset_path", "target_package_path"},
        "asset_place_in_level_plan": {"placements"},
        "asset_place_in_level": {"placements"},
        "asset_validate_level_placements": {"expected_actors"},
    }

    for tool_name, required in expected_required.items():
        schema = _tool_schema(tool_name)
        properties = schema.get("properties") or {}
        assert required.issubset(set(schema.get("required") or [])), tool_name
        assert "ctx" not in properties, tool_name


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


def test_build_asset_import_from_disk_validates_destination_and_passes_dry_run_to_bridge():
    from tools.asset_workflow_tools import build_asset_import_from_disk

    connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "dry_run": True,
                "source_files": ["/tmp/SM_Test.obj"],
                "destination_path": "/Game/UEMCP/Smoke",
                "planned_packages": ["/Game/UEMCP/Smoke/SM_Test"],
                "missing_files": [],
                "conflicts": [],
            },
        }
    )

    envelope = build_asset_import_from_disk(
        lambda: connection,
        source_files=["/tmp/SM_Test.obj", " "],
        destination_path="/Game/UEMCP/Smoke",
        replace_existing=True,
        save_imported_assets=False,
        dry_run=True,
    )

    assert envelope["ok"] is True
    assert connection.calls == [
        (
            "asset_import_from_disk",
            {
                "source_files": ["/tmp/SM_Test.obj"],
                "destination_path": "/Game/UEMCP/Smoke",
                "replace_existing": True,
                "save_imported_assets": False,
                "dry_run": True,
            },
        )
    ]

    bad = build_asset_import_from_disk(
        lambda: connection,
        source_files=["/tmp/SM_Test.obj"],
        destination_path="Content/UEMCP/Smoke",
    )
    assert bad["ok"] is False
    assert bad["error"]["category"] == "invalid_asset_destination_path"


def test_asset_organize_plan_requires_exact_delete_paths_and_reports_operations():
    from tools.asset_workflow_tools import build_asset_organize_plan

    envelope = build_asset_organize_plan(
        operations=[
            {
                "operation": "move",
                "asset_path": "/Game/Imported/SM_Crate",
                "destination_path": "/Game/Organized/SM_Crate",
            }
        ]
    )

    assert envelope["ok"] is True
    assert envelope["data"]["dry_run"] is True
    assert envelope["data"]["operations"][0]["operation"] == "move"

    bad = build_asset_organize_plan(
        operations=[{"operation": "delete", "asset_path": "/Game/Imported/*"}]
    )
    assert bad["ok"] is False
    assert bad["error"]["category"] == "invalid_asset_organize_plan"


def test_asset_mutation_builders_require_game_paths_and_call_expected_commands():
    from tools.asset_workflow_tools import build_asset_move, build_asset_rename

    move_connection = FakeUnrealConnection({"status": "success", "result": {"success": True}})
    move = build_asset_move(
        lambda: move_connection,
        asset_path="/Game/Imported/SM_Crate",
        destination_path="/Game/Organized/SM_Crate",
        dry_run=False,
    )
    assert move["ok"] is True
    assert move_connection.calls == [
        (
            "asset_move",
            {
                "asset_path": "/Game/Imported/SM_Crate",
                "destination_path": "/Game/Organized/SM_Crate",
                "dry_run": False,
            },
        )
    ]

    rename_connection = FakeUnrealConnection({"status": "success", "result": {"success": True}})
    rename = build_asset_rename(
        lambda: rename_connection,
        asset_path="/Game/Imported/SM_Crate",
        new_name="SM_Crate_Organized",
        dry_run=True,
    )
    assert rename["ok"] is True
    assert rename_connection.calls[0][0] == "asset_rename"


def test_asset_prepare_wrapper_and_placement_builders_call_expected_commands():
    from tools.asset_workflow_tools import (
        build_asset_create_blueprint_wrapper,
        build_asset_place_in_level,
        build_asset_place_in_level_plan,
        build_asset_prepare_for_level,
        build_asset_validate_level_placements,
    )

    connection = FakeUnrealConnection({"status": "success", "result": {"ok": True}})

    prepare = build_asset_prepare_for_level(
        lambda: connection,
        asset_path="/Game/Imported/SM_Crate",
    )
    wrapper = build_asset_create_blueprint_wrapper(
        lambda: connection,
        asset_path="/Game/Imported/SM_Crate",
        target_package_path="/Game/Wrappers/BP_Crate",
        dry_run=True,
    )
    plan = build_asset_place_in_level_plan(
        placements=[
            {
                "asset_path": "/Game/Wrappers/BP_Crate",
                "actor_name": "Crate_01",
                "location": [0, 0, 50],
            }
        ],
        target_map="/Game/Maps/L_Test",
    )
    place = build_asset_place_in_level(
        lambda: connection,
        placements=plan["data"]["placements"],
        target_map="/Game/Maps/L_Test",
        dry_run=False,
    )
    validate = build_asset_validate_level_placements(
        lambda: connection,
        expected_actors=["Crate_01"],
        target_map="/Game/Maps/L_Test",
    )

    assert prepare["ok"] is True
    assert wrapper["ok"] is True
    assert plan["ok"] is True
    assert place["ok"] is True
    assert validate["ok"] is True
    assert [call[0] for call in connection.calls] == [
        "asset_prepare_for_level",
        "asset_create_blueprint_wrapper",
        "asset_place_in_level",
        "asset_validate_level_placements",
    ]
