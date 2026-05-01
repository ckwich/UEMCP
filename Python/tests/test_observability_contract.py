from datetime import datetime, timezone

import pytest

from uemcp_observability import (
    build_error_envelope,
    build_success_envelope,
    classify_error,
    unwrap_unreal_response,
)


def test_success_envelope_contains_trace_fields():
    started_at = datetime(2026, 5, 1, 20, 0, 0, tzinfo=timezone.utc)

    envelope = build_success_envelope(
        tool="uemcp_ping",
        started_at=started_at,
        request_id="req-test",
        data={"bridge": {"message": "pong"}},
        warnings=["using cached profile"],
        editor={"project_path": "C:/Dev/Failstate"},
    )

    assert envelope["ok"] is True
    assert envelope["tool"] == "uemcp_ping"
    assert envelope["request_id"] == "req-test"
    assert envelope["started_at"] == "2026-05-01T20:00:00Z"
    assert envelope["finished_at"].endswith("Z")
    assert envelope["duration_ms"] >= 0
    assert envelope["data"] == {"bridge": {"message": "pong"}}
    assert envelope["warnings"] == ["using cached profile"]
    assert envelope["editor"] == {"project_path": "C:/Dev/Failstate"}
    assert envelope["error"] is None


def test_error_envelope_classifies_connection_failures():
    envelope = build_error_envelope(
        tool="get_editor_status",
        message="Failed to connect to Unreal Engine",
        started_at=datetime(2026, 5, 1, 20, 0, 0, tzinfo=timezone.utc),
        request_id="req-error",
    )

    assert envelope["ok"] is False
    assert envelope["tool"] == "get_editor_status"
    assert envelope["data"] is None
    assert envelope["warnings"] == []
    assert envelope["error"] == {
        "category": "connection_failed",
        "message": "Failed to connect to Unreal Engine",
        "raw": None,
    }


@pytest.mark.parametrize(
    ("message", "category"),
    [
        ("Timeout receiving Unreal response", "timeout"),
        ("Unknown command: get_editor_status", "unsupported_command"),
        ("Missing 'path' parameter", "invalid_params"),
        ("Blueprint asset not found", "asset_not_found"),
        ("Automation test failed", "automation_failed"),
        ("Something else", "internal_error"),
    ],
)
def test_classify_error_uses_actionable_categories(message, category):
    assert classify_error(message) == category


def test_unwrap_unreal_success_response_returns_result_payload():
    response = {
        "status": "success",
        "result": {
            "plugin_version": "0.1.0",
            "engine_version": "5.7",
        },
    }

    assert unwrap_unreal_response(response) == {
        "plugin_version": "0.1.0",
        "engine_version": "5.7",
    }


def test_unwrap_unreal_error_response_raises_value_error():
    response = {
        "status": "error",
        "error": "Unknown command: get_editor_status",
    }

    with pytest.raises(ValueError, match="Unknown command"):
        unwrap_unreal_response(response)
