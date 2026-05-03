from uemcp_tool_surface import (
    audit_tool_surface,
    bridge_command_names,
    load_tool_surface_manifest,
    registered_mcp_tool_names,
)


def test_tool_surface_manifest_covers_registered_mcp_tools():
    manifest = load_tool_surface_manifest()

    assert audit_tool_surface(manifest)["missing_mcp_tools"] == []
    assert sorted(manifest["mcp_tools"]) == registered_mcp_tool_names()


def test_tool_surface_manifest_covers_unreal_bridge_commands():
    manifest = load_tool_surface_manifest()

    assert audit_tool_surface(manifest)["missing_bridge_commands"] == []
    assert sorted(manifest["bridge_commands"]) == bridge_command_names()


def test_tool_surface_entries_have_agent_safety_metadata():
    manifest = load_tool_surface_manifest()
    allowed_safety = {
        "read_only",
        "automation_execution",
        "level_mutation",
        "asset_mutation",
        "blueprint_graph_mutation",
        "project_config_mutation",
        "viewport_or_file_output",
        "bridge_only",
    }

    for name, entry in manifest["tools"].items():
        assert entry["category"]
        assert entry["safety"] in allowed_safety, name
        assert entry["exposure"] in {"mcp_registered", "bridge_only"}, name
        assert entry["recommended_use"], name
        assert isinstance(entry["requires_explicit_user_intent"], bool), name


def test_observability_tools_remain_read_only_by_default():
    manifest = load_tool_surface_manifest()

    for name in manifest["tool_groups"]["observability"]:
        entry = manifest["tools"][name]
        assert entry["safety"] in {"read_only", "automation_execution"}, name
        if entry["safety"] == "automation_execution":
            assert entry["requires_explicit_user_intent"] is True
        else:
            assert entry["requires_explicit_user_intent"] is False
