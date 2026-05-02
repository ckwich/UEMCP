from tools.observability_tools import (
    build_automation_test_run,
    build_automation_tests,
    build_editor_automation_readiness_diagnostic,
    build_editor_status,
    build_editor_readiness,
    build_failstate_context,
    build_observability_recent_events,
    build_output_log,
    build_profile_automation_run,
    build_uemcp_ping,
    clear_observability_history,
)


class FakeUnrealConnection:
    def __init__(self, response):
        self.response = response
        self.calls = []

    def send_command(self, command, params=None):
        self.calls.append((command, params or {}))
        return self.response


class SequenceUnrealConnection:
    def __init__(self, responses):
        self.responses = list(responses)
        self.calls = []

    def send_command(self, command, params=None):
        self.calls.append((command, params or {}))
        if not self.responses:
            raise AssertionError(f"Unexpected command: {command}")
        return self.responses.pop(0)


class FakeClock:
    def __init__(self):
        self.now = 0.0
        self.sleeps = []

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


def setup_function():
    clear_observability_history()


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


def test_build_editor_readiness_returns_ready_snapshot_without_waiting():
    connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "project_path": "C:/Dev/Failstate",
                "current_map": "/Game/Failstate/Maps/L_CombatShell_P1",
                "is_pie_running": False,
                "is_slow_task_active": False,
            },
        }
    )

    envelope = build_editor_readiness(lambda: connection)

    assert envelope["ok"] is True
    assert envelope["tool"] == "get_editor_readiness"
    assert envelope["data"]["ready"] is True
    assert envelope["data"]["state"] == "ready"
    assert envelope["data"]["blocking_reasons"] == []
    assert envelope["data"]["latest_status"]["current_map"] == "/Game/Failstate/Maps/L_CombatShell_P1"
    assert envelope["data"]["samples"] == [
        {
            "ok": True,
            "ready": True,
            "request_id": envelope["data"]["samples"][0]["request_id"],
            "current_map": "/Game/Failstate/Maps/L_CombatShell_P1",
            "is_pie_running": False,
            "is_slow_task_active": False,
            "blocking_reasons": [],
            "error": None,
        }
    ]
    assert connection.calls == [("get_editor_status", {})]


def test_build_editor_readiness_reports_busy_reasons_from_status():
    connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "current_map": "/Temp/Untitled_1",
                "is_pie_running": True,
                "is_slow_task_active": True,
            },
        }
    )

    envelope = build_editor_readiness(lambda: connection)

    assert envelope["ok"] is True
    assert envelope["data"]["ready"] is False
    assert envelope["data"]["state"] == "blocked"
    assert envelope["data"]["blocking_reasons"] == [
        "editor_slow_task_active",
        "play_in_editor_running",
    ]


def test_build_editor_readiness_waits_for_stable_ready_samples():
    connection = SequenceUnrealConnection(
        [
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": False,
                    "is_slow_task_active": True,
                },
            },
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": False,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": False,
                    "is_slow_task_active": False,
                },
            },
        ]
    )

    envelope = build_editor_readiness(
        lambda: connection,
        timeout_seconds=1,
        stable_samples=2,
        poll_interval_seconds=0,
    )

    assert envelope["ok"] is True
    assert envelope["data"]["ready"] is True
    assert envelope["data"]["state"] == "ready"
    assert envelope["data"]["total_sample_count"] == 3
    assert [sample["ready"] for sample in envelope["data"]["samples"]] == [False, True, True]
    assert connection.calls == [
        ("get_editor_status", {}),
        ("get_editor_status", {}),
        ("get_editor_status", {}),
    ]


def test_build_editor_readiness_requires_ready_settle_window():
    connection = SequenceUnrealConnection(
        [
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": False,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": False,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": False,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": False,
                    "is_slow_task_active": False,
                },
            },
        ]
    )
    clock = FakeClock()

    envelope = build_editor_readiness(
        lambda: connection,
        timeout_seconds=10,
        stable_samples=2,
        poll_interval_seconds=1,
        settle_seconds=3,
        monotonic=clock.monotonic,
        sleep=clock.sleep,
    )

    assert envelope["ok"] is True
    assert envelope["data"]["ready"] is True
    assert envelope["data"]["state"] == "ready"
    assert envelope["data"]["ready_settle_seconds"] == 3.0
    assert envelope["data"]["total_sample_count"] == 4
    assert clock.sleeps == [1.0, 1.0, 1.0]


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


def test_build_automation_tests_passes_bounded_prefix_query_to_bridge():
    connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "tests": [
                    {
                        "full_test_path": "Failstate.Phase1.Weapon.IntervalMath",
                        "test_name": "Failstate.Phase1.Weapon.IntervalMath",
                    }
                ],
                "truncated": False,
            },
        }
    )

    envelope = build_automation_tests(
        lambda: connection,
        prefix="Failstate.Phase1",
        limit=50,
    )

    assert envelope["ok"] is True
    assert envelope["tool"] == "list_automation_tests"
    assert envelope["data"]["tests"][0]["full_test_path"] == "Failstate.Phase1.Weapon.IntervalMath"
    assert connection.calls == [
        (
            "list_automation_tests",
            {
                "prefix": "Failstate.Phase1",
                "limit": 50,
            },
        )
    ]


def test_build_automation_tests_clamps_limit():
    connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "tests": [],
                "truncated": False,
            },
        }
    )

    envelope = build_automation_tests(lambda: connection, limit=5000)

    assert envelope["ok"] is True
    assert connection.calls == [("list_automation_tests", {"limit": 1000})]


def test_build_automation_test_run_passes_test_name_and_timeout_to_bridge():
    connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "test": {
                    "full_test_path": "UEMCP.Observability.Smoke",
                    "test_name": "UEMCP.Observability.Smoke",
                },
                "status": "passed",
                "successful": True,
                "error_count": 0,
            },
        }
    )

    envelope = build_automation_test_run(
        lambda: connection,
        test_name="UEMCP.Observability.Smoke",
        timeout_seconds=15,
    )

    assert envelope["ok"] is True
    assert envelope["tool"] == "run_automation_test"
    assert envelope["data"]["status"] == "passed"
    assert connection.calls == [
        (
            "run_automation_test",
            {
                "test_name": "UEMCP.Observability.Smoke",
                "timeout_seconds": 15.0,
            },
        )
    ]


def test_build_editor_automation_readiness_diagnostic_returns_ready_summary_with_evidence():
    connection = SequenceUnrealConnection(
        [
            {
                "status": "success",
                "result": {
                    "message": "pong",
                    "plugin_version": "0.1.0",
                    "engine_version": "5.7",
                },
            },
            {
                "status": "success",
                "result": {
                    "project_path": "C:/Dev/Failstate/Failstate.uproject",
                    "current_map": "/Game/Failstate/Maps/L_CombatShell_P1",
                    "is_pie_running": False,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "project_path": "C:/Dev/Failstate/Failstate.uproject",
                    "current_map": "/Game/Failstate/Maps/L_CombatShell_P1",
                    "is_pie_running": False,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "entries": [{"category": "LogTemp", "message": "ready"}],
                    "truncated": False,
                    "matched_entry_count": 1,
                },
            },
        ]
    )

    envelope = build_editor_automation_readiness_diagnostic(
        lambda: connection,
        readiness_stable_samples=1,
        output_log_limit=2,
    )

    data = envelope["data"]
    assert envelope["ok"] is True
    assert envelope["tool"] == "diagnose_editor_automation_readiness"
    assert data["ready_for_automation"] is True
    assert data["summary"] == {
        "state": "ready",
        "first_blocking_category": None,
        "first_blocking_message": None,
    }
    assert data["gates"]["bridge"]["ok"] is True
    assert data["gates"]["status"]["ok"] is True
    assert data["gates"]["readiness"]["ready"] is True
    assert data["gates"]["output_log"]["ok"] is True
    assert data["gates"]["output_log"]["entry_count"] == 1
    assert data["observability_events"] == []
    assert data["evidence_refs"]["ping_request_id"].startswith("uemcp_ping-")
    assert data["evidence_refs"]["status_request_id"].startswith("get_editor_status-")
    assert data["evidence_refs"]["readiness_request_id"].startswith("get_editor_readiness-")
    assert data["evidence_refs"]["output_log_request_id"].startswith("get_output_log-")
    assert connection.calls == [
        ("ping", {}),
        ("get_editor_status", {}),
        ("get_editor_status", {}),
        ("get_output_log", {"limit": 2}),
    ]


def test_build_editor_automation_readiness_diagnostic_reports_editor_busy_event():
    connection = SequenceUnrealConnection(
        [
            {"status": "success", "result": {"message": "pong"}},
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": True,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": True,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "entries": [],
                    "truncated": False,
                    "matched_entry_count": 0,
                },
            },
        ]
    )

    envelope = build_editor_automation_readiness_diagnostic(
        lambda: connection,
        output_log_limit=1,
    )

    data = envelope["data"]
    event = data["observability_events"][0]
    assert envelope["ok"] is True
    assert data["ready_for_automation"] is False
    assert data["summary"]["state"] == "blocked"
    assert data["summary"]["first_blocking_category"] == "editor_busy"
    assert data["gates"]["readiness"]["failure_category"] == "editor_busy"
    assert data["gates"]["readiness"]["blocking_reasons"] == ["play_in_editor_running"]
    assert event["type"] == "readiness_gate_failed"
    assert event["phase"] == "diagnostic_readiness"
    assert event["failure_category"] == "editor_busy"
    assert event["blocking_reasons"] == ["play_in_editor_running"]


def test_build_editor_automation_readiness_diagnostic_preserves_connection_failure():
    envelope = build_editor_automation_readiness_diagnostic(
        lambda: None,
        output_log_limit=1,
    )

    data = envelope["data"]
    event = data["observability_events"][0]
    assert envelope["ok"] is True
    assert data["ready_for_automation"] is False
    assert data["summary"]["state"] == "blocked"
    assert data["summary"]["first_blocking_category"] == "connection_failed"
    assert data["gates"]["bridge"]["ok"] is False
    assert data["gates"]["status"]["ok"] is False
    assert data["gates"]["readiness"]["failure_category"] == "connection_failed"
    assert data["gates"]["output_log"]["ok"] is False
    assert event["phase"] == "diagnostic_readiness"
    assert event["failure_category"] == "connection_failed"
    assert data["evidence_refs"]["ping_request_id"].startswith("uemcp_ping-")
    assert data["evidence_refs"]["status_request_id"].startswith("get_editor_status-")
    assert data["evidence_refs"]["readiness_request_id"].startswith("get_editor_readiness-")
    assert data["evidence_refs"]["output_log_request_id"].startswith("get_output_log-")


def test_build_observability_recent_events_returns_compact_history_without_bridge_calls():
    connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "project_path": "C:/Dev/Failstate/Failstate.uproject",
                "current_map": "/Game/Failstate/Maps/L_CombatShell_P1",
                "is_pie_running": False,
                "is_slow_task_active": False,
            },
        }
    )

    readiness = build_editor_readiness(lambda: connection)
    call_count_before_history_read = len(connection.calls)
    history = build_observability_recent_events(limit=5)

    assert readiness["ok"] is True
    assert history["ok"] is True
    assert history["tool"] == "get_observability_recent_events"
    assert len(connection.calls) == call_count_before_history_read

    data = history["data"]
    assert data["returned_count"] == 1
    assert data["total_recorded"] == 1
    assert data["history_capacity"] == 100
    assert data["filters"] == {
        "tool": None,
        "limit": 5,
        "include_success": True,
        "newest_first": True,
    }
    entry = data["entries"][0]
    assert entry["sequence"] == 1
    assert entry["tool"] == "get_editor_readiness"
    assert entry["request_id"] == readiness["request_id"]
    assert entry["ok"] is True
    assert entry["successful"] is True
    assert entry["summary"] == {
        "ready": True,
        "state": "ready",
        "blocking_reasons": [],
    }
    assert entry["observability_events"] == []
    assert entry["evidence_refs"] == {
        "readiness_request_id": readiness["request_id"],
        "status_request_ids": [readiness["data"]["samples"][0]["request_id"]],
    }


def test_build_observability_recent_events_filters_blocked_diagnostics():
    ready_connection = FakeUnrealConnection(
        {
            "status": "success",
            "result": {
                "current_map": "/Game/Failstate/Maps/L_CombatShell_P1",
                "is_pie_running": False,
                "is_slow_task_active": False,
            },
        }
    )
    blocked_connection = SequenceUnrealConnection(
        [
            {"status": "success", "result": {"message": "pong"}},
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": True,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": True,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "entries": [],
                    "truncated": False,
                    "matched_entry_count": 0,
                },
            },
        ]
    )

    build_editor_readiness(lambda: ready_connection)
    diagnostic = build_editor_automation_readiness_diagnostic(
        lambda: blocked_connection,
        output_log_limit=1,
    )

    history = build_observability_recent_events(
        tool="diagnose_editor_automation_readiness",
        include_success=False,
    )

    data = history["data"]
    assert data["returned_count"] == 1
    assert data["total_recorded"] == 3
    assert data["filters"]["tool"] == "diagnose_editor_automation_readiness"
    assert data["filters"]["include_success"] is False

    entry = data["entries"][0]
    assert entry["tool"] == "diagnose_editor_automation_readiness"
    assert entry["request_id"] == diagnostic["request_id"]
    assert entry["ok"] is True
    assert entry["successful"] is False
    assert entry["failure_category"] == "editor_busy"
    assert entry["message"] == "Editor is not ready for automation: play_in_editor_running"
    assert entry["summary"] == {
        "state": "blocked",
        "first_blocking_category": "editor_busy",
        "first_blocking_message": "Editor is not ready for automation: play_in_editor_running",
        "ready_for_automation": False,
    }
    assert entry["observability_events"] == diagnostic["data"]["observability_events"]
    assert entry["evidence_refs"] == diagnostic["data"]["evidence_refs"]


def test_build_observability_recent_events_records_profile_preflight_failures():
    profile_run = build_profile_automation_run(
        lambda: None,
        profile_name="failstate",
        limit=2,
        output_log_limit=0,
    )

    history = build_observability_recent_events(
        tool="run_profile_automation_tests",
        include_success=False,
    )

    assert profile_run["ok"] is False
    data = history["data"]
    assert data["returned_count"] == 1
    assert data["total_recorded"] == 2

    entry = data["entries"][0]
    assert entry["tool"] == "run_profile_automation_tests"
    assert entry["request_id"] == profile_run["request_id"]
    assert entry["ok"] is False
    assert entry["successful"] is False
    assert entry["failure_category"] == "connection_failed"
    assert "connection_failed" in entry["message"]
    assert entry["summary"] == {
        "ready": False,
        "state": "blocked",
        "blocking_reasons": ["editor_status_unavailable"],
    }
    assert entry["observability_events"] == profile_run["error"]["raw"]["observability_events"]
    assert entry["evidence_refs"] == {
        "readiness_request_id": profile_run["error"]["raw"]["readiness_request_id"],
        "status_request_ids": [profile_run["error"]["raw"]["samples"][0]["request_id"]],
    }


def test_build_profile_automation_run_blocks_when_editor_is_not_ready():
    connection = SequenceUnrealConnection(
        [
            {
                "status": "success",
                "result": {
                    "current_map": "/Temp/Untitled_1",
                    "is_pie_running": False,
                    "is_slow_task_active": True,
                },
            },
        ]
    )

    envelope = build_profile_automation_run(
        lambda: connection,
        profile_name="failstate",
        limit=2,
        output_log_limit=0,
    )

    assert envelope["ok"] is False
    assert envelope["tool"] == "run_profile_automation_tests"
    assert envelope["error"]["category"] == "editor_busy"
    assert "not ready" in envelope["error"]["message"].lower()
    assert envelope["error"]["raw"]["blocking_reasons"] == ["editor_slow_task_active"]
    assert envelope["error"]["raw"]["readiness_request_id"].startswith("get_editor_readiness-")
    assert envelope["error"]["raw"]["failure_category"] == "editor_busy"
    assert envelope["error"]["raw"]["observability_events"] == [
        {
            "type": "readiness_gate_failed",
            "phase": "profile_automation_preflight",
            "severity": "error",
            "failure_category": "editor_busy",
            "state": "blocked",
            "blocking_reasons": ["editor_slow_task_active"],
            "message": "Editor is not ready for automation: editor_slow_task_active",
            "readiness_request_id": envelope["error"]["raw"]["readiness_request_id"],
            "latest_status_request_id": envelope["error"]["raw"]["samples"][0]["request_id"],
            "status_error_category": None,
            "evidence_refs": {
                "readiness_request_id": envelope["error"]["raw"]["readiness_request_id"],
                "status_request_ids": [envelope["error"]["raw"]["samples"][0]["request_id"]],
            },
        }
    ]
    assert connection.calls == [("get_editor_status", {})]


def test_build_profile_automation_run_preserves_connection_failure_readiness_event():
    envelope = build_profile_automation_run(
        lambda: None,
        profile_name="failstate",
        limit=2,
        output_log_limit=0,
    )

    assert envelope["ok"] is False
    assert envelope["tool"] == "run_profile_automation_tests"
    assert envelope["error"]["category"] == "connection_failed"
    assert "connection_failed" in envelope["error"]["message"]
    assert envelope["error"]["raw"]["blocking_reasons"] == ["editor_status_unavailable"]
    assert envelope["error"]["raw"]["failure_category"] == "connection_failed"

    event = envelope["error"]["raw"]["observability_events"][0]
    assert event["type"] == "readiness_gate_failed"
    assert event["phase"] == "profile_automation_preflight"
    assert event["failure_category"] == "connection_failed"
    assert event["blocking_reasons"] == ["editor_status_unavailable"]
    assert event["status_error_category"] == "connection_failed"
    assert event["readiness_request_id"].startswith("get_editor_readiness-")
    assert event["latest_status_request_id"].startswith("get_editor_status-")
    assert event["evidence_refs"] == {
        "readiness_request_id": event["readiness_request_id"],
        "status_request_ids": [event["latest_status_request_id"]],
    }


def test_build_profile_automation_run_executes_profile_prefix_batch_with_summary_and_log_tail():
    connection = SequenceUnrealConnection(
        [
            {
                "status": "success",
                "result": {
                    "current_map": "/Game/Failstate/Maps/L_CombatShell_P1",
                    "is_pie_running": False,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "tests": [
                        {
                            "full_test_path": "Failstate.Phase1.Project.RequiredModulesAvailable",
                            "test_name": "FFailstateProjectSmokeTest",
                            "display_name": "Required modules",
                        },
                        {
                            "full_test_path": "Failstate.Phase1.Project.CoreAssetsLoad",
                            "test_name": "FFailstateProjectAssetSmokeTest",
                            "display_name": "Core assets",
                        },
                    ],
                    "matched_test_count": 2,
                    "returned_test_count": 2,
                    "truncated": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "test": {
                        "full_test_path": "Failstate.Phase1.Project.RequiredModulesAvailable",
                        "test_name": "FFailstateProjectSmokeTest",
                    },
                    "status": "passed",
                    "successful": True,
                    "duration_seconds": 0.1,
                    "error_count": 0,
                    "warning_count": 0,
                    "events": [],
                },
            },
            {
                "status": "success",
                "result": {
                    "test": {
                        "full_test_path": "Failstate.Phase1.Project.CoreAssetsLoad",
                        "test_name": "FFailstateProjectAssetSmokeTest",
                    },
                    "status": "failed",
                    "successful": False,
                    "duration_seconds": 0.2,
                    "error_count": 1,
                    "warning_count": 0,
                    "events": [
                        {
                            "type": "error",
                            "message": "Missing asset",
                            "context": "Failstate",
                        }
                    ],
                },
            },
            {
                "status": "success",
                "result": {
                    "entries": [{"category": "LogAutomationTest", "message": "Missing asset"}],
                    "truncated": False,
                    "matched_entry_count": 1,
                },
            },
        ]
    )

    envelope = build_profile_automation_run(
        lambda: connection,
        profile_name="failstate",
        limit=2,
        timeout_seconds=12,
        output_log_limit=3,
    )

    assert envelope["ok"] is True
    assert envelope["tool"] == "run_profile_automation_tests"
    assert envelope["data"]["readiness"]["ready"] is True
    assert envelope["data"]["observability_events"] == []
    assert envelope["data"]["evidence_refs"]["readiness_request_id"].startswith(
        "get_editor_readiness-"
    )
    assert envelope["data"]["mode"] == "prefix"
    assert envelope["data"]["prefix"] == "Failstate.Phase1"
    assert envelope["data"]["summary"] == {
        "total": 2,
        "passed": 1,
        "failed": 1,
        "errors": 0,
        "timed_out": 0,
        "successful": False,
    }
    assert envelope["data"]["tests"][0]["status"] == "passed"
    assert envelope["data"]["tests"][1]["status"] == "failed"
    assert envelope["data"]["tests"][1]["event_snippets"] == [
        {
            "type": "error",
            "message": "Missing asset",
            "context": "Failstate",
        }
    ]
    assert envelope["data"]["output_log_tail"]["entries"] == [
        {"category": "LogAutomationTest", "message": "Missing asset"}
    ]
    assert connection.calls == [
        ("get_editor_status", {}),
        ("list_automation_tests", {"limit": 2, "prefix": "Failstate.Phase1"}),
        (
            "run_automation_test",
            {
                "test_name": "Failstate.Phase1.Project.RequiredModulesAvailable",
                "timeout_seconds": 12.0,
            },
        ),
        (
            "run_automation_test",
            {
                "test_name": "Failstate.Phase1.Project.CoreAssetsLoad",
                "timeout_seconds": 12.0,
            },
        ),
        ("get_output_log", {"limit": 3}),
    ]


def test_build_profile_automation_run_executes_one_exact_test_without_discovery():
    connection = SequenceUnrealConnection(
        [
            {
                "status": "success",
                "result": {
                    "current_map": "/Game/Failstate/Maps/L_CombatShell_P1",
                    "is_pie_running": False,
                    "is_slow_task_active": False,
                },
            },
            {
                "status": "success",
                "result": {
                    "test": {
                        "full_test_path": "UEMCP.Observability.Smoke",
                        "test_name": "FUEMCPObservabilitySmokeTest",
                    },
                    "status": "passed",
                    "successful": True,
                    "duration_seconds": 0.01,
                    "error_count": 0,
                    "warning_count": 0,
                    "events": [],
                },
            },
            {
                "status": "success",
                "result": {
                    "entries": [],
                    "truncated": False,
                    "matched_entry_count": 0,
                },
            },
        ]
    )

    envelope = build_profile_automation_run(
        lambda: connection,
        test_name="UEMCP.Observability.Smoke",
        timeout_seconds=5,
        output_log_limit=1,
    )

    assert envelope["ok"] is True
    assert envelope["data"]["mode"] == "single"
    assert envelope["data"]["readiness"]["ready"] is True
    assert envelope["data"]["observability_events"] == []
    assert envelope["data"]["requested_test_name"] == "UEMCP.Observability.Smoke"
    assert envelope["data"]["discovery"] is None
    assert envelope["data"]["summary"]["successful"] is True
    assert connection.calls == [
        ("get_editor_status", {}),
        (
            "run_automation_test",
            {
                "test_name": "UEMCP.Observability.Smoke",
                "timeout_seconds": 5.0,
            },
        ),
        ("get_output_log", {"limit": 1}),
    ]


def test_build_failstate_context_returns_profile_envelope():
    envelope = build_failstate_context()

    assert envelope["ok"] is True
    assert envelope["tool"] == "get_failstate_context"
    assert envelope["data"]["active_profile"] == "failstate"
    assert envelope["data"]["read_only"] is True
