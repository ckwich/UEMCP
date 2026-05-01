from tools.observability_tools import (
    build_editor_status,
    build_failstate_context,
    build_output_log,
    build_uemcp_ping,
)


class FakeUnrealConnection:
    def __init__(self, response):
        self.response = response
        self.calls = []

    def send_command(self, command, params=None):
        self.calls.append((command, params or {}))
        return self.response


def test_build_uemcp_ping_returns_ok_envelope_for_bridge_success():
    connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "message": "pong",
                "plugin_version": "0.1.0",
                "engine_version": "5.7",
            },
        }
    )

    envelope = build_uemcp_ping(lambda: connection)

    assert envelope["ok"] is True
    assert envelope["tool"] == "uemcp_ping"
    assert envelope["data"]["bridge"]["message"] == "pong"
    assert envelope["data"]["server"]["name"] == "UEMCP"
    assert connection.calls == [("ping", {})]


def test_build_uemcp_ping_returns_connection_error_without_bridge():
    envelope = build_uemcp_ping(lambda: None)

    assert envelope["ok"] is False
    assert envelope["tool"] == "uemcp_ping"
    assert envelope["error"]["category"] == "connection_failed"
    assert "Unable to connect" in envelope["error"]["message"]


def test_build_editor_status_unwraps_status_payload():
    connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "project_path": "C:/Dev/Failstate",
                "engine_version": "5.7",
                "current_map": "/Game/Maps/Phase1",
            },
        }
    )

    envelope = build_editor_status(lambda: connection)

    assert envelope["ok"] is True
    assert envelope["data"]["project_path"] == "C:/Dev/Failstate"
    assert envelope["editor"]["project_path"] == "C:/Dev/Failstate"
    assert connection.calls == [("get_editor_status", {})]


def test_build_output_log_passes_bounded_filters_to_bridge():
    connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "entries": [{"category": "LogTemp", "message": "ready"}],
                "truncated": False,
            },
        }
    )

    envelope = build_output_log(
        lambda: connection,
        limit=25,
        category="LogTemp",
        verbosity="Warning",
        contains="ready",
    )

    assert envelope["ok"] is True
    assert envelope["data"]["entries"] == [{"category": "LogTemp", "message": "ready"}]
    assert connection.calls == [
        (
            "get_output_log",
            {
                "limit": 25,
                "category": "LogTemp",
                "verbosity": "Warning",
                "contains": "ready",
            },
        )
    ]


def test_build_failstate_context_returns_profile_envelope():
    envelope = build_failstate_context()

    assert envelope["ok"] is True
    assert envelope["tool"] == "get_failstate_context"
    assert envelope["data"]["active_profile"] == "failstate"
    assert envelope["data"]["read_only"] is True
