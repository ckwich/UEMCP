from unreal_mcp_server import UnrealConnection


def test_receive_timeout_for_automation_run_includes_runtime_budget():
    connection = UnrealConnection()

    assert connection.receive_timeout_for_command(
        "run_automation_test",
        {"timeout_seconds": 120},
    ) == 135.0


def test_receive_timeout_for_short_commands_uses_cold_editor_budget():
    connection = UnrealConnection()

    assert connection.receive_timeout_for_command("list_automation_tests", {"limit": 20}) == 30.0
