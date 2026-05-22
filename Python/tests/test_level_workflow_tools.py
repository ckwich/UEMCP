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


def test_level_workflow_tools_are_registered_without_context():
    expected_required = {
        "level_list_maps": {"roots"},
        "level_create": {"package_path"},
        "level_open": {"package_path"},
        "level_save": set(),
        "level_construction_plan": {"operations"},
        "level_apply_construction_plan": {"operations"},
        "level_validate_construction": {"expected_actors"},
    }

    for tool_name, required in expected_required.items():
        schema = _tool_schema(tool_name)
        properties = schema.get("properties") or {}
        assert required.issubset(set(schema.get("required") or [])), tool_name
        assert "ctx" not in properties, tool_name


def test_level_lifecycle_builders_validate_paths_and_call_bridge():
    from tools.level_workflow_tools import (
        build_level_create,
        build_level_list_maps,
        build_level_open,
        build_level_save,
    )

    connection = FakeUnrealConnection({"status": "success", "result": {"ok": True}})

    list_maps = build_level_list_maps(lambda: connection, roots=["/Game/Maps", " "], limit=20000)
    create = build_level_create(
        lambda: connection,
        package_path="/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
        save_existing=True,
        save_new_level=True,
        dry_run=True,
    )
    open_level = build_level_open(
        lambda: connection,
        package_path="/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
        save_existing=False,
    )
    save = build_level_save(lambda: connection, only_if_dirty=False)

    assert list_maps["ok"] is True
    assert create["ok"] is True
    assert open_level["ok"] is True
    assert save["ok"] is True
    assert connection.calls == [
        (
            "level_list_maps",
            {"roots": ["/Game/Maps"], "limit": 10000},
        ),
        (
            "level_create",
            {
                "package_path": "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
                "template_path": None,
                "save_existing": True,
                "save_new_level": True,
                "fail_if_exists": True,
                "dry_run": True,
            },
        ),
        (
            "level_open",
            {
                "package_path": "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
                "save_existing": False,
                "require_exists": True,
            },
        ),
        (
            "level_save",
            {
                "package_path": None,
                "only_if_dirty": False,
                "include_external_actor_packages": True,
            },
        ),
    ]

    bad_create = build_level_create(lambda: connection, package_path="Content/Maps/L_Test")
    assert bad_create["ok"] is False
    assert bad_create["error"]["category"] == "invalid_level_package_path"

    bad_open = build_level_open(lambda: connection, package_path="/Game/Maps/*")
    assert bad_open["ok"] is False
    assert bad_open["error"]["category"] == "invalid_level_package_path"


def test_level_construction_plan_normalizes_operations_and_refuses_unsafe_inputs():
    from tools.level_workflow_tools import build_level_construction_plan

    envelope = build_level_construction_plan(
        target_map="/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
        operations=[
            {
                "op": "ensure_actor",
                "actor_name": "UEMCP_LevelSmoke_PointLight",
                "actor_class": "/Script/Engine.PointLight",
                "label": "UEMCP Level Smoke Point Light",
                "folder_path": "UEMCP/Smoke",
                "location": [0, 0, 180],
                "tags": ["UEMCPSmoke", " "],
            }
        ],
    )

    assert envelope["ok"] is True
    assert envelope["data"]["target_map"] == "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke"
    assert envelope["data"]["operation_count"] == 1
    operation = envelope["data"]["operations"][0]
    assert operation == {
        "op": "ensure_actor",
        "actor_name": "UEMCP_LevelSmoke_PointLight",
        "actor_class": "/Script/Engine.PointLight",
        "asset_path": None,
        "label": "UEMCP Level Smoke Point Light",
        "folder_path": "UEMCP/Smoke",
        "location": [0.0, 0.0, 180.0],
        "rotation": [0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 1.0],
        "tags": ["UEMCPSmoke"],
        "confirm_delete": False,
    }

    missing_actor = build_level_construction_plan(
        operations=[{"op": "ensure_actor", "actor_class": "/Script/Engine.PointLight"}]
    )
    assert missing_actor["ok"] is False
    assert missing_actor["error"]["category"] == "invalid_level_construction_plan"

    unsafe_delete = build_level_construction_plan(
        operations=[{"op": "delete_actor", "actor_name": "PointLight_1"}]
    )
    assert unsafe_delete["ok"] is False
    assert unsafe_delete["error"]["category"] == "invalid_level_construction_plan"


def test_level_apply_construction_plan_calls_bridge_with_normalized_operations():
    from tools.level_workflow_tools import build_level_apply_construction_plan

    connection = FakeUnrealConnection({"status": "success", "result": {"applied": True}})

    envelope = build_level_apply_construction_plan(
        lambda: connection,
        target_map="/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
        operations=[
            {
                "op": "ensure_actor",
                "actor_name": "UEMCP_LevelSmoke_PointLight",
                "actor_class": "/Script/Engine.PointLight",
                "location": [0, 0, 180],
            }
        ],
        open_level=True,
        create_if_missing=False,
        save_level=True,
        dry_run=False,
    )

    assert envelope["ok"] is True
    assert connection.calls == [
        (
            "level_apply_construction_plan",
            {
                "target_map": "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
                "operations": [
                    {
                        "op": "ensure_actor",
                        "actor_name": "UEMCP_LevelSmoke_PointLight",
                        "actor_class": "/Script/Engine.PointLight",
                        "asset_path": None,
                        "label": "",
                        "folder_path": "",
                        "location": [0.0, 0.0, 180.0],
                        "rotation": [0.0, 0.0, 0.0],
                        "scale": [1.0, 1.0, 1.0],
                        "tags": [],
                        "confirm_delete": False,
                    }
                ],
                "open_level": True,
                "create_if_missing": False,
                "save_level": True,
                "dry_run": False,
            },
        )
    ]


def test_level_validate_construction_requires_expected_actors_and_normalizes_evidence():
    from tools.level_workflow_tools import build_level_validate_construction

    connection = FakeUnrealConnection({"status": "success", "result": {"passed": True}})

    envelope = build_level_validate_construction(
        lambda: connection,
        target_map="/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
        expected_actors=[
            {
                "actor_name": "UEMCP_LevelSmoke_PointLight",
                "class": "PointLight",
                "folder_path": "UEMCP/Smoke",
                "tags": ["UEMCPSmoke"],
                "location": [0, 0, 180],
            }
        ],
        location_tolerance=0.1,
    )

    assert envelope["ok"] is True
    assert connection.calls == [
        (
            "level_validate_construction",
            {
                "target_map": "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
                "expected_actors": [
                    {
                        "actor_name": "UEMCP_LevelSmoke_PointLight",
                        "class": "PointLight",
                        "label": "",
                        "folder_path": "UEMCP/Smoke",
                        "tags": ["UEMCPSmoke"],
                        "location": [0.0, 0.0, 180.0],
                    }
                ],
                "location_tolerance": 0.1,
            },
        )
    ]

    bad = build_level_validate_construction(lambda: connection, expected_actors=[])
    assert bad["ok"] is False
    assert bad["error"]["category"] == "invalid_level_expected_actors"

    bad_map = build_level_validate_construction(
        lambda: connection,
        target_map="/Game/Maps/*",
        expected_actors=[{"actor_name": "A"}],
    )
    assert bad_map["ok"] is False
    assert bad_map["error"]["category"] == "invalid_level_target_map"
