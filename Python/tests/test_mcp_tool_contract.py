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


def test_pie_runtime_snapshot_is_exposed_as_separate_observability_tool():
    schema = _tool_schema("get_pie_runtime_snapshot")

    properties = schema.get("properties") or {}
    assert {
        "pie_instance_index",
        "limit",
        "class_name",
        "name_contains",
        "include_components",
        "component_limit",
    }.issubset(set(properties))
    assert "pie_instance_index" not in (schema.get("required") or [])


def test_asset_intake_snapshot_routes_to_asset_workflow_command_handler():
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
    command_source = (
        repo_root
        / "MCPGameProject"
        / "Plugins"
        / "UnrealMCP"
        / "Source"
        / "UnrealMCP"
        / "Private"
        / "Commands"
        / "UnrealMCPAssetWorkflowCommands.cpp"
    ).read_text(encoding="utf-8")

    assert 'CommandType == TEXT("asset_intake_snapshot")' in bridge_source
    assert "AssetWorkflowCommandsForTask->HandleCommand" in bridge_source
    assert "IAssetRegistry" in command_source
    assert "HandleAssetIntakeSnapshot" in command_source


def test_asset_workflow_bridge_routes_import_organization_and_placement_commands():
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
    command_source = (
        repo_root
        / "MCPGameProject"
        / "Plugins"
        / "UnrealMCP"
        / "Source"
        / "UnrealMCP"
        / "Private"
        / "Commands"
        / "UnrealMCPAssetWorkflowCommands.cpp"
    ).read_text(encoding="utf-8")

    for command_name in [
        "asset_import_from_disk",
        "asset_rename",
        "asset_move",
        "asset_duplicate",
        "asset_delete",
        "asset_save_packages",
        "asset_fixup_redirectors",
        "asset_prepare_for_level",
        "asset_create_blueprint_wrapper",
        "asset_place_in_level",
        "asset_validate_level_placements",
    ]:
        assert f'CommandType == TEXT("{command_name}")' in bridge_source
        assert f'CommandType == TEXT("{command_name}")' in command_source

    assert "UAssetImportTask" in command_source
    assert "UEditorAssetSubsystem" in command_source
    assert "FixupReferencers" in command_source
    assert "UStaticMesh" in command_source


def test_level_workflow_bridge_routes_lifecycle_and_construction_commands():
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
    command_source = (
        repo_root
        / "MCPGameProject"
        / "Plugins"
        / "UnrealMCP"
        / "Source"
        / "UnrealMCP"
        / "Private"
        / "Commands"
        / "UnrealMCPLevelWorkflowCommands.cpp"
    ).read_text(encoding="utf-8")

    for command_name in [
        "level_list_maps",
        "level_create",
        "level_open",
        "level_save",
        "level_apply_construction_plan",
        "level_validate_construction",
    ]:
        assert f'CommandType == TEXT("{command_name}")' in bridge_source
        assert f'CommandType == TEXT("{command_name}")' in command_source

    assert "LevelWorkflowCommandsForTask->HandleCommand" in bridge_source
    assert "UEditorLoadingAndSavingUtils" in command_source
    assert "UEditorActorSubsystem" in command_source
    assert "SavePackages" in command_source


def test_bridge_schedules_editor_commands_on_ticker_instead_of_nested_game_thread_task():
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

    assert "FTSTicker::GetCoreTicker().AddTicker" in bridge_source
    assert "AsyncTask(ENamedThreads::GameThread" not in bridge_source


def test_pie_runtime_snapshot_bridge_targets_pie_worlds_explicitly():
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

    assert 'CommandType == TEXT("get_pie_runtime_snapshot")' in bridge_source
    assert "EWorldType::PIE" in editor_source
    assert "GetWorldContexts()" in editor_source


def test_socket_bridge_accumulates_json_and_sends_utf8_bytes():
    repo_root = Path(__file__).resolve().parents[2]
    server_source = (
        repo_root
        / "MCPGameProject"
        / "Plugins"
        / "UnrealMCP"
        / "Source"
        / "UnrealMCP"
        / "Private"
        / "MCPServerRunnable.cpp"
    ).read_text(encoding="utf-8")
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

    assert "ReadCompleteJsonMessage" in server_source
    assert "SendUtf8Response" in server_source
    assert "FTCHARToUTF8" in server_source
    assert "TotalBytesSent < ResponseBytes.Length()" in server_source
    assert "Buffer[BytesRead] = " not in server_source
    assert "GetObjectField(TEXT(\"params\"))" not in server_source
    assert "IsInGameThread()" in bridge_source
    assert "DestroySocket(ConnectionSocket.Get())" not in bridge_source
    assert "DestroySocket(ListenerSocket.Get())" not in bridge_source


def test_unreal_bridge_is_module_owned_and_ping_does_not_wait_for_editor_startup():
    repo_root = Path(__file__).resolve().parents[2]
    module_source = (
        repo_root
        / "MCPGameProject"
        / "Plugins"
        / "UnrealMCP"
        / "Source"
        / "UnrealMCP"
        / "Private"
        / "UnrealMCPModule.cpp"
    ).read_text(encoding="utf-8")
    bridge_header = (
        repo_root
        / "MCPGameProject"
        / "Plugins"
        / "UnrealMCP"
        / "Source"
        / "UnrealMCP"
        / "Public"
        / "UnrealMCPBridge.h"
    ).read_text(encoding="utf-8")
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

    assert "Bridge = MakeUnique<FUnrealMCPBridge>()" in module_source
    assert "Bridge->StartServer()" in module_source
    assert "UEditorSubsystem" not in bridge_header
    assert "CommandType == TEXT(\"ping\")" in bridge_source
    assert "!GIsRunning || !GEditor" in bridge_source
    assert "GetGameThreadCommandTimeoutSeconds" in bridge_source
    assert "Future.WaitFor" in bridge_source
    assert "bCommandCancelled->Store(true)" in bridge_source
    assert "Command was cancelled before reaching the Unreal editor game thread" in bridge_source
    assert "EditorCommandsForTask = EditorCommands" in bridge_source
    assert "[this, CommandType, Params]" not in bridge_source
    assert "Unreal Editor is still starting up" in bridge_source


def test_editor_actor_commands_use_editor_world_transactions():
    repo_root = Path(__file__).resolve().parents[2]
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

    assert "ResolveEditorWorld" in editor_source
    assert "FindActorByName" in editor_source
    assert "FScopedTransaction" in editor_source
    assert "UEditorActorSubsystem" in editor_source
    assert "UGameplayStatics::GetAllActorsOfClass(GWorld" not in editor_source


def test_set_component_property_uses_property_value_addresses():
    repo_root = Path(__file__).resolve().parents[2]
    blueprint_source = (
        repo_root
        / "MCPGameProject"
        / "Plugins"
        / "UnrealMCP"
        / "Source"
        / "UnrealMCP"
        / "Private"
        / "Commands"
        / "UnrealMCPBlueprintCommands.cpp"
    ).read_text(encoding="utf-8")

    assert "EnumPropertyAddr = EnumProp->ContainerPtrToValuePtr<void>(ComponentTemplate)" in blueprint_source
    assert "NumericPropertyAddr = NumericProp->ContainerPtrToValuePtr<void>(ComponentTemplate)" in blueprint_source
    assert "SetIntPropertyValue(ComponentTemplate" not in blueprint_source
    assert "SetFloatingPointPropertyValue(ComponentTemplate" not in blueprint_source


def test_get_actor_properties_bridge_serializes_bounded_reflection_by_default():
    repo_root = Path(__file__).resolve().parents[2]
    common_utils_source = (
        repo_root
        / "MCPGameProject"
        / "Plugins"
        / "UnrealMCP"
        / "Source"
        / "UnrealMCP"
        / "Private"
        / "Commands"
        / "UnrealMCPCommonUtils.cpp"
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

    assert "TFieldIterator<FProperty>" in common_utils_source
    assert "EFieldIteratorFlags::IncludeSuper" in common_utils_source
    assert "FStructProperty" in common_utils_source
    assert "ExportTextItem_Direct" in common_utils_source
    assert "FActorPropertySerializationOptions" in common_utils_source
    assert "ShouldIncludeReflectedProperty" in common_utils_source
    assert "MaxActorPropertyEntries" in common_utils_source
    assert "include_private" in editor_source
    assert "property_limit" in editor_source
    assert 'SetObjectField(TEXT("properties")' in common_utils_source


def test_python_editor_tools_do_not_swallow_bridge_failures_as_empty_results():
    repo_root = Path(__file__).resolve().parents[2]
    editor_tools_source = (repo_root / "Python" / "tools" / "editor_tools.py").read_text(encoding="utf-8")

    assert "bridge_error_response" in editor_tools_source
    assert "return []" not in editor_tools_source
    assert "return {}" not in editor_tools_source


def test_server_prompt_does_not_advertise_unregistered_viewport_tools():
    repo_root = Path(__file__).resolve().parents[2]
    server_source = (repo_root / "Python" / "unreal_mcp_server.py").read_text(encoding="utf-8")

    prompt_text = server_source.split("def info():", 1)[1]
    assert "focus_viewport(" not in prompt_text
    assert "take_screenshot(" not in prompt_text
