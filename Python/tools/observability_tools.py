"""Read-mostly observability tools for UEMCP."""

from __future__ import annotations

import logging
import time
from collections import deque
from threading import Lock
from typing import Any, Callable, Deque, Dict, List, Optional

from mcp.server.fastmcp import Context, FastMCP

from uemcp_observability import (
    build_error_envelope,
    build_success_envelope,
    execute_bridge_command,
    format_timestamp,
    get_failstate_context_data,
    server_metadata,
    utc_now,
)

logger = logging.getLogger("UnrealMCP")

MAX_PROFILE_AUTOMATION_RUNS = 50
MAX_EVENT_SNIPPETS = 5
MAX_READINESS_SAMPLES = 5
MAX_OBSERVABILITY_HISTORY_EVENTS = 100

_OBSERVABILITY_HISTORY_LOCK = Lock()
_OBSERVABILITY_HISTORY: Deque[Dict[str, Any]] = deque(
    maxlen=MAX_OBSERVABILITY_HISTORY_EVENTS
)
_OBSERVABILITY_HISTORY_SEQUENCE = 0
_HISTORY_RECORDED_TOOLS = {
    "get_editor_readiness",
    "diagnose_editor_automation_readiness",
    "run_profile_automation_tests",
}


def _default_connection_factory():
    from unreal_mcp_server import get_unreal_connection

    return get_unreal_connection()


def _editor_identity(data: Dict[str, Any]) -> Dict[str, Any]:
    return {
        key: data[key]
        for key in ("engine_version", "project_path", "plugin_version", "current_map")
        if key in data
    }


def _first_automation_prefix(profile: Dict[str, Any]) -> str:
    prefixes = profile.get("automation_test_prefixes") or []
    return str(prefixes[0]) if prefixes else ""


def _automation_test_identifier(test: Dict[str, Any]) -> str:
    return str(test.get("full_test_path") or test.get("test_name") or "")


def _event_snippets(events: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    snippets: List[Dict[str, Any]] = []
    for event in events[:MAX_EVENT_SNIPPETS]:
        snippet = {
            "type": event.get("type"),
            "message": event.get("message"),
            "context": event.get("context"),
        }
        snippets.append({key: value for key, value in snippet.items() if value not in (None, "")})
    return snippets


def clear_observability_history() -> None:
    global _OBSERVABILITY_HISTORY_SEQUENCE
    with _OBSERVABILITY_HISTORY_LOCK:
        _OBSERVABILITY_HISTORY.clear()
        _OBSERVABILITY_HISTORY_SEQUENCE = 0


def _observability_events_from_envelope(envelope: Dict[str, Any]) -> List[Dict[str, Any]]:
    data = envelope.get("data")
    if isinstance(data, dict):
        events = data.get("observability_events")
        if isinstance(events, list):
            return [dict(event) for event in events if isinstance(event, dict)]

    error = envelope.get("error")
    if isinstance(error, dict):
        raw = error.get("raw")
        if isinstance(raw, dict):
            events = raw.get("observability_events")
            if isinstance(events, list):
                return [dict(event) for event in events if isinstance(event, dict)]

    return []


def _history_status_request_ids(raw: Dict[str, Any]) -> List[str]:
    request_ids: List[str] = []
    for sample in raw.get("samples") or []:
        if not isinstance(sample, dict):
            continue
        request_id = sample.get("request_id")
        if request_id:
            request_ids.append(str(request_id))
    return request_ids


def _history_evidence_refs(
    envelope: Dict[str, Any],
    events: List[Dict[str, Any]],
) -> Dict[str, Any]:
    data = envelope.get("data")
    if isinstance(data, dict):
        evidence_refs = data.get("evidence_refs")
        if isinstance(evidence_refs, dict):
            return dict(evidence_refs)
        if envelope.get("tool") == "get_editor_readiness":
            return {
                "readiness_request_id": envelope.get("request_id"),
                "status_request_ids": _history_status_request_ids(data),
            }

    error = envelope.get("error")
    if isinstance(error, dict):
        raw = error.get("raw")
        if isinstance(raw, dict):
            evidence_refs = raw.get("evidence_refs")
            if isinstance(evidence_refs, dict):
                return dict(evidence_refs)
            readiness_request_id = raw.get("readiness_request_id")
            status_request_ids = _history_status_request_ids(raw)
            if readiness_request_id or status_request_ids:
                return {
                    "readiness_request_id": readiness_request_id,
                    "status_request_ids": status_request_ids,
                }

    for event in events:
        evidence_refs = event.get("evidence_refs")
        if isinstance(evidence_refs, dict):
            return dict(evidence_refs)

    return {"request_id": envelope.get("request_id")}


def _history_summary(envelope: Dict[str, Any]) -> Dict[str, Any]:
    tool = envelope.get("tool")
    data = envelope.get("data")
    if isinstance(data, dict):
        if tool == "get_editor_readiness":
            return {
                "ready": bool(data.get("ready", False)),
                "state": data.get("state"),
                "blocking_reasons": list(data.get("blocking_reasons") or []),
            }
        if tool == "diagnose_editor_automation_readiness":
            summary = dict(data.get("summary") or {})
            summary["ready_for_automation"] = bool(data.get("ready_for_automation", False))
            return summary
        if tool == "run_profile_automation_tests":
            return dict(data.get("summary") or {})

    error = envelope.get("error")
    if isinstance(error, dict):
        raw = error.get("raw")
        if isinstance(raw, dict):
            return {
                "ready": bool(raw.get("ready", False)),
                "state": raw.get("state"),
                "blocking_reasons": list(raw.get("blocking_reasons") or []),
            }

    return {}


def _history_failure_category(
    envelope: Dict[str, Any],
    summary: Dict[str, Any],
    events: List[Dict[str, Any]],
) -> Optional[str]:
    error = envelope.get("error")
    if isinstance(error, dict):
        category = error.get("category")
        return str(category) if category else None

    for event in events:
        category = event.get("failure_category")
        if category:
            return str(category)

    tool = envelope.get("tool")
    data = envelope.get("data")
    if tool == "diagnose_editor_automation_readiness":
        category = summary.get("first_blocking_category")
        return str(category) if category else None
    if tool == "get_editor_readiness" and isinstance(data, dict) and not data.get("ready"):
        readiness = {
            "state": data.get("state"),
            "blocking_reasons": list(data.get("blocking_reasons") or []),
            "samples": list(data.get("samples") or []),
        }
        return _readiness_failure_category(readiness)
    if tool == "run_profile_automation_tests" and summary.get("successful") is False:
        return "automation_failed"

    return None


def _history_message(
    envelope: Dict[str, Any],
    summary: Dict[str, Any],
    events: List[Dict[str, Any]],
) -> Optional[str]:
    error = envelope.get("error")
    if isinstance(error, dict):
        message = error.get("message")
        return str(message) if message else None

    message = summary.get("first_blocking_message")
    if message:
        return str(message)

    for event in events:
        event_message = event.get("message")
        if event_message:
            return str(event_message)

    tool = envelope.get("tool")
    if tool == "get_editor_readiness":
        return "Editor is ready for automation" if summary.get("ready") else "Editor is not ready"
    if tool == "diagnose_editor_automation_readiness":
        return (
            "Editor is ready for automation"
            if summary.get("ready_for_automation")
            else "Editor automation readiness diagnostic is blocked"
        )
    if tool == "run_profile_automation_tests":
        return (
            "Profile automation run completed successfully"
            if summary.get("successful")
            else "Profile automation run completed with failures"
        )
    return None


def _history_successful(envelope: Dict[str, Any], summary: Dict[str, Any]) -> bool:
    if not envelope.get("ok"):
        return False
    tool = envelope.get("tool")
    if tool == "get_editor_readiness":
        return bool(summary.get("ready"))
    if tool == "diagnose_editor_automation_readiness":
        return bool(summary.get("ready_for_automation"))
    if tool == "run_profile_automation_tests":
        return bool(summary.get("successful"))
    return True


def _history_entry_from_envelope(envelope: Dict[str, Any]) -> Dict[str, Any]:
    summary = _history_summary(envelope)
    events = _observability_events_from_envelope(envelope)
    failure_category = _history_failure_category(envelope, summary, events)
    return {
        "recorded_at": format_timestamp(utc_now()),
        "tool": envelope.get("tool"),
        "request_id": envelope.get("request_id"),
        "ok": bool(envelope.get("ok")),
        "successful": _history_successful(envelope, summary),
        "failure_category": failure_category,
        "message": _history_message(envelope, summary, events),
        "summary": summary,
        "observability_events": events,
        "evidence_refs": _history_evidence_refs(envelope, events),
        "editor": dict(envelope.get("editor") or {}),
        "warnings": list(envelope.get("warnings") or []),
    }


def _record_observability_envelope(envelope: Dict[str, Any]) -> Dict[str, Any]:
    if envelope.get("tool") not in _HISTORY_RECORDED_TOOLS:
        return envelope

    global _OBSERVABILITY_HISTORY_SEQUENCE
    entry = _history_entry_from_envelope(envelope)
    with _OBSERVABILITY_HISTORY_LOCK:
        _OBSERVABILITY_HISTORY_SEQUENCE += 1
        entry["sequence"] = _OBSERVABILITY_HISTORY_SEQUENCE
        _OBSERVABILITY_HISTORY.append(entry)
    return envelope


def _compact_run_result(run_envelope: Dict[str, Any], requested_test_name: str) -> Dict[str, Any]:
    if not run_envelope.get("ok"):
        return {
            "requested_test_name": requested_test_name,
            "full_test_path": requested_test_name,
            "test_name": requested_test_name,
            "status": "error",
            "successful": False,
            "duration_seconds": None,
            "error_count": 1,
            "warning_count": 0,
            "event_snippets": [],
            "run_request_id": run_envelope.get("request_id"),
            "error": run_envelope.get("error"),
        }

    data = dict(run_envelope.get("data") or {})
    test = dict(data.get("test") or {})
    return {
        "requested_test_name": requested_test_name,
        "full_test_path": str(test.get("full_test_path") or requested_test_name),
        "test_name": str(test.get("test_name") or requested_test_name),
        "display_name": test.get("display_name"),
        "status": str(data.get("status") or "unknown"),
        "successful": bool(data.get("successful")),
        "timed_out": bool(data.get("timed_out", False)),
        "duration_seconds": data.get("duration_seconds"),
        "error_count": int(data.get("error_count") or 0),
        "warning_count": int(data.get("warning_count") or 0),
        "event_snippets": _event_snippets(list(data.get("events") or [])),
        "run_request_id": run_envelope.get("request_id"),
        "error": None,
    }


def _automation_summary(results: List[Dict[str, Any]]) -> Dict[str, Any]:
    total = len(results)
    passed = sum(1 for result in results if result["status"] == "passed" and result["successful"])
    errors = sum(1 for result in results if result["status"] == "error")
    timed_out = sum(1 for result in results if result.get("timed_out") or result["status"] == "timed_out")
    failed = sum(1 for result in results if not result["successful"] and result["status"] != "error")
    return {
        "total": total,
        "passed": passed,
        "failed": failed,
        "errors": errors,
        "timed_out": timed_out,
        "successful": total > 0 and passed == total,
    }


def _compact_readiness_result(readiness_envelope: Dict[str, Any]) -> Dict[str, Any]:
    readiness_data = dict(readiness_envelope.get("data") or {})
    return {
        "readiness_request_id": readiness_envelope.get("request_id"),
        "ready": bool(readiness_data.get("ready", False)),
        "state": readiness_data.get("state"),
        "blocking_reasons": list(readiness_data.get("blocking_reasons") or []),
        "latest_status": dict(readiness_data.get("latest_status") or {}),
        "samples": list(readiness_data.get("samples") or []),
        "total_sample_count": readiness_data.get("total_sample_count"),
        "stable_samples_required": readiness_data.get("stable_samples_required"),
        "ready_settle_seconds": readiness_data.get("ready_settle_seconds"),
        "timeout_seconds": readiness_data.get("timeout_seconds"),
        "poll_interval_seconds": readiness_data.get("poll_interval_seconds"),
    }


def _latest_readiness_status_error_category(readiness: Dict[str, Any]) -> Optional[str]:
    samples = list(readiness.get("samples") or [])
    latest_sample = dict(samples[-1]) if samples else {}
    error = latest_sample.get("error")
    if isinstance(error, dict):
        category = error.get("category")
        return str(category) if category else None
    return None


def _readiness_failure_category(readiness: Dict[str, Any]) -> str:
    status_error_category = _latest_readiness_status_error_category(readiness)
    if status_error_category:
        return status_error_category

    if readiness.get("state") == "timeout":
        return "timeout"

    blocking_reasons = set(readiness.get("blocking_reasons") or [])
    if blocking_reasons.intersection({"editor_slow_task_active", "play_in_editor_running"}):
        return "editor_busy"
    return "internal_error"


def _readiness_status_request_ids(readiness: Dict[str, Any]) -> List[str]:
    request_ids: List[str] = []
    for sample in readiness.get("samples") or []:
        request_id = sample.get("request_id")
        if request_id:
            request_ids.append(str(request_id))
    return request_ids


def _readiness_failure_event(
    readiness: Dict[str, Any],
    *,
    phase: str = "profile_automation_preflight",
) -> Dict[str, Any]:
    blocking_reasons = list(readiness.get("blocking_reasons") or ["editor_status_unavailable"])
    status_request_ids = _readiness_status_request_ids(readiness)
    failure_category = _readiness_failure_category(readiness)
    if failure_category == "editor_busy":
        message = "Editor is not ready for automation: " + ", ".join(blocking_reasons)
    else:
        message = (
            f"Editor readiness gate failed: {failure_category}: "
            + ", ".join(blocking_reasons)
        )

    return {
        "type": "readiness_gate_failed",
        "phase": phase,
        "severity": "error",
        "failure_category": failure_category,
        "state": readiness.get("state"),
        "blocking_reasons": blocking_reasons,
        "message": message,
        "readiness_request_id": readiness.get("readiness_request_id"),
        "latest_status_request_id": status_request_ids[-1] if status_request_ids else None,
        "status_error_category": _latest_readiness_status_error_category(readiness),
        "evidence_refs": {
            "readiness_request_id": readiness.get("readiness_request_id"),
            "status_request_ids": status_request_ids,
        },
    }


def _error_category(envelope: Dict[str, Any]) -> Optional[str]:
    error = envelope.get("error")
    if isinstance(error, dict):
        category = error.get("category")
        return str(category) if category else None
    return None


def _error_message(envelope: Dict[str, Any]) -> Optional[str]:
    error = envelope.get("error")
    if isinstance(error, dict):
        message = error.get("message")
        return str(message) if message else None
    return None


def _diagnostic_gate(
    envelope: Dict[str, Any],
    *,
    name: str,
    message: str,
    data: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    gate = {
        "name": name,
        "ok": bool(envelope.get("ok")),
        "request_id": envelope.get("request_id"),
        "category": None,
        "message": None,
    }
    if gate["ok"]:
        gate["message"] = message
    else:
        gate["category"] = _error_category(envelope)
        gate["message"] = _error_message(envelope)
    gate.update(data or {})
    return gate


def _diagnostic_gate_event(gate: Dict[str, Any], *, phase: str) -> Dict[str, Any]:
    category = gate.get("category") or "internal_error"
    message = gate.get("message") or f"Diagnostic gate failed: {gate.get('name')}"
    return {
        "type": "diagnostic_gate_failed",
        "phase": phase,
        "severity": "error",
        "failure_category": category,
        "gate": gate.get("name"),
        "message": message,
        "request_id": gate.get("request_id"),
        "evidence_refs": {
            f"{gate.get('name')}_request_id": gate.get("request_id"),
        },
    }


def _readiness_reasons(status_data: Dict[str, Any]) -> List[str]:
    reasons: List[str] = []
    if status_data.get("is_slow_task_active"):
        reasons.append("editor_slow_task_active")
    if status_data.get("is_pie_running"):
        reasons.append("play_in_editor_running")
    return reasons


def _readiness_sample(status_envelope: Dict[str, Any]) -> Dict[str, Any]:
    if not status_envelope.get("ok"):
        return {
            "ok": False,
            "ready": False,
            "request_id": status_envelope.get("request_id"),
            "current_map": None,
            "is_pie_running": None,
            "is_slow_task_active": None,
            "blocking_reasons": ["editor_status_unavailable"],
            "error": status_envelope.get("error"),
        }

    status_data = dict(status_envelope.get("data") or {})
    reasons = _readiness_reasons(status_data)
    return {
        "ok": True,
        "ready": not reasons,
        "request_id": status_envelope.get("request_id"),
        "current_map": status_data.get("current_map"),
        "is_pie_running": bool(status_data.get("is_pie_running", False)),
        "is_slow_task_active": bool(status_data.get("is_slow_task_active", False)),
        "blocking_reasons": reasons,
        "error": None,
    }


def build_uemcp_ping(
    connection_factory: Callable[[], Any] = _default_connection_factory,
) -> Dict[str, Any]:
    return execute_bridge_command(
        tool="uemcp_ping",
        command="ping",
        connection_factory=connection_factory,
        data_builder=lambda bridge: {
            "server": server_metadata(),
            "bridge": bridge,
        },
        editor_builder=_editor_identity,
    )


def build_editor_status(
    connection_factory: Callable[[], Any] = _default_connection_factory,
) -> Dict[str, Any]:
    return execute_bridge_command(
        tool="get_editor_status",
        command="get_editor_status",
        connection_factory=connection_factory,
        editor_builder=_editor_identity,
    )


def build_editor_readiness(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    timeout_seconds: float = 0.0,
    stable_samples: int = 1,
    poll_interval_seconds: float = 1.0,
    settle_seconds: float = 0.0,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> Dict[str, Any]:
    started_at = utc_now()
    bounded_timeout_seconds = max(0.0, min(float(timeout_seconds), 300.0))
    bounded_stable_samples = max(1, min(int(stable_samples), 10))
    bounded_poll_interval_seconds = max(0.0, min(float(poll_interval_seconds), 10.0))
    bounded_settle_seconds = max(0.0, min(float(settle_seconds), 120.0))

    deadline = monotonic() + bounded_timeout_seconds
    consecutive_ready_samples = 0
    ready_since_seconds: Optional[float] = None
    total_sample_count = 0
    samples: List[Dict[str, Any]] = []
    latest_status: Dict[str, Any] = {}
    latest_sample: Dict[str, Any] = {
        "ready": False,
        "blocking_reasons": ["editor_status_unavailable"],
    }

    while True:
        status_envelope = build_editor_status(connection_factory)
        now_seconds = monotonic()
        latest_status = dict(status_envelope.get("data") or {})
        latest_sample = _readiness_sample(status_envelope)
        total_sample_count += 1
        samples.append(latest_sample)
        samples = samples[-MAX_READINESS_SAMPLES:]

        if latest_sample["ready"]:
            consecutive_ready_samples += 1
            if ready_since_seconds is None:
                ready_since_seconds = now_seconds
            has_stable_samples = consecutive_ready_samples >= bounded_stable_samples
            has_settled = (now_seconds - ready_since_seconds) >= bounded_settle_seconds
            if has_stable_samples and has_settled:
                state = "ready"
                ready = True
                break
        else:
            consecutive_ready_samples = 0
            ready_since_seconds = None

        if bounded_timeout_seconds == 0.0 or monotonic() >= deadline:
            ready = False
            state = "timeout" if bounded_timeout_seconds > 0.0 else "blocked"
            break

        sleep(bounded_poll_interval_seconds)

    data = {
        "ready": ready,
        "state": state,
        "blocking_reasons": list(latest_sample.get("blocking_reasons") or []),
        "latest_status": latest_status,
        "samples": samples,
        "total_sample_count": total_sample_count,
        "stable_samples_required": bounded_stable_samples,
        "ready_settle_seconds": bounded_settle_seconds,
        "timeout_seconds": bounded_timeout_seconds,
        "poll_interval_seconds": bounded_poll_interval_seconds,
    }

    warnings = []
    if state == "timeout":
        warnings.append("Timed out waiting for editor readiness")

    return _record_observability_envelope(
        build_success_envelope(
            tool="get_editor_readiness",
            started_at=started_at,
            data=data,
            warnings=warnings,
            editor=_editor_identity(latest_status),
        )
    )


def build_output_log(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    limit: int = 200,
    category: Optional[str] = None,
    verbosity: Optional[str] = None,
    contains: Optional[str] = None,
) -> Dict[str, Any]:
    bounded_limit = max(1, min(int(limit), 1000))
    params: Dict[str, Any] = {"limit": bounded_limit}
    if category:
        params["category"] = category
    if verbosity:
        params["verbosity"] = verbosity
    if contains:
        params["contains"] = contains

    return execute_bridge_command(
        tool="get_output_log",
        command="get_output_log",
        connection_factory=connection_factory,
        params=params,
    )


def build_automation_tests(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    prefix: Optional[str] = None,
    limit: int = 200,
) -> Dict[str, Any]:
    bounded_limit = max(1, min(int(limit), 1000))
    params: Dict[str, Any] = {"limit": bounded_limit}
    if prefix:
        params["prefix"] = prefix

    return execute_bridge_command(
        tool="list_automation_tests",
        command="list_automation_tests",
        connection_factory=connection_factory,
        params=params,
    )


def build_automation_test_run(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    test_name: str,
    timeout_seconds: float = 30.0,
) -> Dict[str, Any]:
    bounded_timeout_seconds = max(1.0, min(float(timeout_seconds), 120.0))
    return execute_bridge_command(
        tool="run_automation_test",
        command="run_automation_test",
        connection_factory=connection_factory,
        params={
            "test_name": test_name,
            "timeout_seconds": bounded_timeout_seconds,
        },
    )


def build_editor_automation_readiness_diagnostic(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    readiness_timeout_seconds: float = 0.0,
    readiness_stable_samples: int = 1,
    readiness_poll_interval_seconds: float = 1.0,
    readiness_settle_seconds: float = 0.0,
    output_log_limit: int = 5,
) -> Dict[str, Any]:
    started_at = utc_now()
    bounded_output_log_limit = max(0, min(int(output_log_limit), 1000))

    ping_envelope = build_uemcp_ping(connection_factory)
    status_envelope = build_editor_status(connection_factory)
    readiness_envelope = build_editor_readiness(
        connection_factory,
        timeout_seconds=readiness_timeout_seconds,
        stable_samples=readiness_stable_samples,
        poll_interval_seconds=readiness_poll_interval_seconds,
        settle_seconds=readiness_settle_seconds,
    )

    if bounded_output_log_limit:
        output_log_envelope = build_output_log(
            connection_factory,
            limit=bounded_output_log_limit,
        )
        output_log_data = dict(output_log_envelope.get("data") or {})
        output_log_gate = _diagnostic_gate(
            output_log_envelope,
            name="output_log",
            message="Output log capture is available",
            data={
                "entry_count": len(output_log_data.get("entries") or []),
                "matched_entry_count": output_log_data.get("matched_entry_count"),
                "truncated": output_log_data.get("truncated"),
                "skipped": False,
            },
        )
        output_log_request_id = output_log_envelope.get("request_id")
    else:
        output_log_gate = {
            "name": "output_log",
            "ok": True,
            "request_id": None,
            "category": None,
            "message": "Output log capture skipped",
            "entry_count": None,
            "matched_entry_count": None,
            "truncated": None,
            "skipped": True,
        }
        output_log_request_id = None

    status_data = dict(status_envelope.get("data") or {})
    readiness = _compact_readiness_result(readiness_envelope)
    readiness_ready = bool(readiness.get("ready"))
    readiness_gate = {
        "name": "readiness",
        "ok": bool(readiness_envelope.get("ok")),
        "request_id": readiness.get("readiness_request_id"),
        "ready": readiness_ready,
        "state": readiness.get("state"),
        "blocking_reasons": list(readiness.get("blocking_reasons") or []),
        "failure_category": None if readiness_ready else _readiness_failure_category(readiness),
        "message": "Editor is ready for automation" if readiness_ready else None,
        "latest_status_request_id": (
            _readiness_status_request_ids(readiness)[-1]
            if _readiness_status_request_ids(readiness)
            else None
        ),
    }

    bridge_gate = _diagnostic_gate(
        ping_envelope,
        name="bridge",
        message="Editor bridge is reachable",
    )
    status_gate = _diagnostic_gate(
        status_envelope,
        name="status",
        message="Editor status is available",
        data={
            "current_map": status_data.get("current_map"),
            "is_pie_running": status_data.get("is_pie_running"),
            "is_slow_task_active": status_data.get("is_slow_task_active"),
        },
    )

    observability_events: List[Dict[str, Any]] = []
    if not readiness_ready:
        readiness_event = _readiness_failure_event(readiness, phase="diagnostic_readiness")
        readiness_gate["message"] = readiness_event["message"]
        observability_events.append(readiness_event)

    gate_order = [bridge_gate, status_gate, readiness_gate, output_log_gate]
    first_blocking_category = None
    first_blocking_message = None
    for gate in gate_order:
        gate_blocks = not gate.get("ok") or (
            gate.get("name") == "readiness" and not gate.get("ready")
        )
        if gate_blocks:
            first_blocking_category = (
                gate.get("failure_category") or gate.get("category") or "internal_error"
            )
            first_blocking_message = gate.get("message")
            break

    if first_blocking_category is None and not output_log_gate.get("ok"):
        first_blocking_category = output_log_gate.get("category") or "internal_error"
        first_blocking_message = output_log_gate.get("message")

    if not output_log_gate.get("ok"):
        observability_events.append(
            _diagnostic_gate_event(output_log_gate, phase="diagnostic_output_log")
        )

    ready_for_automation = (
        bool(bridge_gate.get("ok"))
        and bool(status_gate.get("ok"))
        and readiness_ready
        and bool(output_log_gate.get("ok"))
    )
    if ready_for_automation:
        state = "ready"
    elif readiness_ready and not output_log_gate.get("ok"):
        state = "degraded"
    else:
        state = "blocked"

    data = {
        "ready_for_automation": ready_for_automation,
        "summary": {
            "state": state,
            "first_blocking_category": first_blocking_category,
            "first_blocking_message": first_blocking_message,
        },
        "gates": {
            "bridge": bridge_gate,
            "status": status_gate,
            "readiness": readiness_gate,
            "output_log": output_log_gate,
        },
        "readiness": readiness,
        "observability_events": observability_events,
        "evidence_refs": {
            "ping_request_id": ping_envelope.get("request_id"),
            "status_request_id": status_envelope.get("request_id"),
            "readiness_request_id": readiness.get("readiness_request_id"),
            "output_log_request_id": output_log_request_id,
        },
    }

    return _record_observability_envelope(
        build_success_envelope(
            tool="diagnose_editor_automation_readiness",
            started_at=started_at,
            data=data,
            editor=_editor_identity(status_data or readiness.get("latest_status") or {}),
        )
    )


def build_observability_recent_events(
    *,
    tool: Optional[str] = None,
    limit: int = 20,
    include_success: bool = True,
    newest_first: bool = True,
) -> Dict[str, Any]:
    started_at = utc_now()
    bounded_limit = max(1, min(int(limit), MAX_OBSERVABILITY_HISTORY_EVENTS))

    with _OBSERVABILITY_HISTORY_LOCK:
        snapshot = [dict(entry) for entry in _OBSERVABILITY_HISTORY]

    filtered_entries = [
        entry
        for entry in snapshot
        if (tool is None or entry.get("tool") == tool)
        and (include_success or not entry.get("successful"))
    ]
    if newest_first:
        filtered_entries = list(reversed(filtered_entries))
    entries = filtered_entries[:bounded_limit]

    data = {
        "entries": entries,
        "returned_count": len(entries),
        "total_recorded": len(snapshot),
        "history_capacity": MAX_OBSERVABILITY_HISTORY_EVENTS,
        "filters": {
            "tool": tool,
            "limit": bounded_limit,
            "include_success": bool(include_success),
            "newest_first": bool(newest_first),
        },
    }

    return build_success_envelope(
        tool="get_observability_recent_events",
        started_at=started_at,
        data=data,
    )


def build_profile_automation_run(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    profile_name: str = "failstate",
    test_name: Optional[str] = None,
    prefix: Optional[str] = None,
    limit: int = 10,
    timeout_seconds: float = 30.0,
    output_log_limit: int = 25,
    require_ready: bool = True,
    readiness_timeout_seconds: float = 0.0,
    readiness_stable_samples: int = 1,
    readiness_poll_interval_seconds: float = 1.0,
    readiness_settle_seconds: float = 0.0,
) -> Dict[str, Any]:
    started_at = utc_now()
    context = get_failstate_context_data(profile_name)
    profile = dict(context["profile"])
    warnings = list(context["warnings"])

    bounded_timeout_seconds = max(1.0, min(float(timeout_seconds), 120.0))
    bounded_limit = max(1, min(int(limit), MAX_PROFILE_AUTOMATION_RUNS))
    bounded_output_log_limit = max(0, min(int(output_log_limit), 1000))

    mode = "single" if test_name else "prefix"
    resolved_prefix = prefix or _first_automation_prefix(profile)
    if mode == "prefix" and not resolved_prefix:
        return _record_observability_envelope(
            build_error_envelope(
                tool="run_profile_automation_tests",
                started_at=started_at,
                message=f"Profile '{profile_name}' does not define an automation test prefix",
                category="invalid_params",
                warnings=warnings,
            )
        )

    readiness = None
    if require_ready:
        readiness_envelope = build_editor_readiness(
            connection_factory,
            timeout_seconds=readiness_timeout_seconds,
            stable_samples=readiness_stable_samples,
            poll_interval_seconds=readiness_poll_interval_seconds,
            settle_seconds=readiness_settle_seconds,
        )
        readiness = _compact_readiness_result(readiness_envelope)
        if not readiness["ready"]:
            readiness_event = _readiness_failure_event(readiness)
            raw_readiness = dict(readiness)
            raw_readiness["failure_category"] = readiness_event["failure_category"]
            raw_readiness["observability_events"] = [readiness_event]
            return _record_observability_envelope(
                build_error_envelope(
                    tool="run_profile_automation_tests",
                    started_at=started_at,
                    message=readiness_event["message"],
                    category=readiness_event["failure_category"],
                    raw=raw_readiness,
                    warnings=warnings + list(readiness_envelope.get("warnings") or []),
                    editor=_editor_identity(readiness["latest_status"]),
                )
            )
        warnings.extend(readiness_envelope.get("warnings") or [])

    discovery = None
    tests_to_run: List[Dict[str, Any]]
    if test_name:
        tests_to_run = [{"full_test_path": test_name, "test_name": test_name}]
    else:
        discovery_envelope = build_automation_tests(
            connection_factory,
            prefix=resolved_prefix,
            limit=bounded_limit,
        )
        if not discovery_envelope.get("ok"):
            return _record_observability_envelope(
                build_error_envelope(
                    tool="run_profile_automation_tests",
                    started_at=started_at,
                    message=f"Failed to discover automation tests for prefix '{resolved_prefix}'",
                    category="automation_failed",
                    raw=discovery_envelope.get("error"),
                    warnings=warnings,
                )
            )

        discovery_data = dict(discovery_envelope.get("data") or {})
        tests_to_run = list(discovery_data.get("tests") or [])
        discovery = {
            "request_id": discovery_envelope.get("request_id"),
            "matched_test_count": discovery_data.get("matched_test_count"),
            "returned_test_count": discovery_data.get("returned_test_count"),
            "truncated": discovery_data.get("truncated"),
            "filters": discovery_data.get("filters") or {},
        }
        if discovery.get("truncated"):
            warnings.append(
                f"Automation discovery for prefix '{resolved_prefix}' was truncated to {bounded_limit} tests"
            )
        if not tests_to_run:
            warnings.append(f"No automation tests matched prefix '{resolved_prefix}'")

    results: List[Dict[str, Any]] = []
    run_request_ids: List[str] = []
    for test in tests_to_run:
        requested_test_name = _automation_test_identifier(test)
        if not requested_test_name:
            warnings.append(f"Skipping automation test with no runnable name: {test}")
            continue

        run_envelope = build_automation_test_run(
            connection_factory,
            test_name=requested_test_name,
            timeout_seconds=bounded_timeout_seconds,
        )
        run_request_ids.append(str(run_envelope.get("request_id")))
        results.append(_compact_run_result(run_envelope, requested_test_name))

    output_log_tail = None
    output_log_request_id = None
    if bounded_output_log_limit:
        output_log_envelope = build_output_log(
            connection_factory,
            limit=bounded_output_log_limit,
        )
        output_log_request_id = output_log_envelope.get("request_id")
        if output_log_envelope.get("ok"):
            output_log_data = dict(output_log_envelope.get("data") or {})
            output_log_tail = {
                "request_id": output_log_request_id,
                "entries": output_log_data.get("entries") or [],
                "truncated": output_log_data.get("truncated"),
                "matched_entry_count": output_log_data.get("matched_entry_count"),
            }
        else:
            warnings.append(f"Failed to capture output log tail: {output_log_envelope.get('error')}")

    data = {
        "profile_name": profile_name,
        "mode": mode,
        "prefix": resolved_prefix if mode == "prefix" else prefix,
        "requested_test_name": test_name,
        "limit": bounded_limit if mode == "prefix" else None,
        "timeout_seconds": bounded_timeout_seconds,
        "readiness": readiness,
        "observability_events": [],
        "summary": _automation_summary(results),
        "tests": results,
        "discovery": discovery,
        "output_log_tail": output_log_tail,
        "evidence_refs": {
            "readiness_request_id": readiness["readiness_request_id"] if readiness else None,
            "discovery_request_id": discovery["request_id"] if discovery else None,
            "run_request_ids": run_request_ids,
            "output_log_request_id": output_log_request_id,
        },
    }

    return _record_observability_envelope(
        build_success_envelope(
            tool="run_profile_automation_tests",
            started_at=started_at,
            data=data,
            warnings=warnings,
        )
    )


def build_failstate_context(profile_name: str = "failstate") -> Dict[str, Any]:
    started_at = utc_now()
    context = get_failstate_context_data(profile_name)
    return build_success_envelope(
        tool="get_failstate_context",
        started_at=started_at,
        data=context,
        warnings=context["warnings"],
    )


def register_observability_tools(mcp: FastMCP):
    """Register read-mostly observability tools."""

    @mcp.tool()
    def uemcp_ping(ctx: Context) -> Dict[str, Any]:
        """Check MCP server, bridge, and Unreal Editor reachability."""
        return build_uemcp_ping()

    @mcp.tool()
    def get_editor_status(ctx: Context) -> Dict[str, Any]:
        """Return read-only Unreal Editor status and project identity."""
        return build_editor_status()

    @mcp.tool()
    def get_editor_readiness(
        ctx: Context,
        timeout_seconds: float = 0.0,
        stable_samples: int = 1,
        poll_interval_seconds: float = 1.0,
        settle_seconds: float = 0.0,
    ) -> Dict[str, Any]:
        """Return whether the editor is ready for automation, optionally waiting for idle samples."""
        return build_editor_readiness(
            timeout_seconds=timeout_seconds,
            stable_samples=stable_samples,
            poll_interval_seconds=poll_interval_seconds,
            settle_seconds=settle_seconds,
        )

    @mcp.tool()
    def diagnose_editor_automation_readiness(
        ctx: Context,
        readiness_timeout_seconds: float = 0.0,
        readiness_stable_samples: int = 1,
        readiness_poll_interval_seconds: float = 1.0,
        readiness_settle_seconds: float = 0.0,
        output_log_limit: int = 5,
    ) -> Dict[str, Any]:
        """Return a compact read-only diagnostic summary before automation runs."""
        return build_editor_automation_readiness_diagnostic(
            readiness_timeout_seconds=readiness_timeout_seconds,
            readiness_stable_samples=readiness_stable_samples,
            readiness_poll_interval_seconds=readiness_poll_interval_seconds,
            readiness_settle_seconds=readiness_settle_seconds,
            output_log_limit=output_log_limit,
        )

    @mcp.tool()
    def get_observability_recent_events(
        ctx: Context,
        tool: Optional[str] = None,
        limit: int = 20,
        include_success: bool = True,
        newest_first: bool = True,
    ) -> Dict[str, Any]:
        """Return recent high-level observability results without querying the editor."""
        return build_observability_recent_events(
            tool=tool,
            limit=limit,
            include_success=include_success,
            newest_first=newest_first,
        )

    @mcp.tool()
    def get_output_log(
        ctx: Context,
        limit: int = 200,
        category: Optional[str] = None,
        verbosity: Optional[str] = None,
        contains: Optional[str] = None,
    ) -> Dict[str, Any]:
        """Return bounded Unreal output log entries when the editor bridge supports them."""
        return build_output_log(
            limit=limit,
            category=category,
            verbosity=verbosity,
            contains=contains,
        )

    @mcp.tool()
    def list_automation_tests(
        ctx: Context,
        prefix: Optional[str] = None,
        limit: int = 200,
    ) -> Dict[str, Any]:
        """List Unreal automation tests by optional dot-path prefix."""
        return build_automation_tests(prefix=prefix, limit=limit)

    @mcp.tool()
    def run_automation_test(
        ctx: Context,
        test_name: str,
        timeout_seconds: float = 30.0,
    ) -> Dict[str, Any]:
        """Run one exact Unreal automation test and return structured results."""
        return build_automation_test_run(
            test_name=test_name,
            timeout_seconds=timeout_seconds,
        )

    @mcp.tool()
    def run_profile_automation_tests(
        ctx: Context,
        profile_name: str = "failstate",
        test_name: Optional[str] = None,
        prefix: Optional[str] = None,
        limit: int = 10,
        timeout_seconds: float = 30.0,
        output_log_limit: int = 25,
        require_ready: bool = True,
        readiness_timeout_seconds: float = 0.0,
        readiness_stable_samples: int = 1,
        readiness_poll_interval_seconds: float = 1.0,
        readiness_settle_seconds: float = 0.0,
    ) -> Dict[str, Any]:
        """Run one automation test or a bounded profile prefix batch with a compact summary."""
        return build_profile_automation_run(
            profile_name=profile_name,
            test_name=test_name,
            prefix=prefix,
            limit=limit,
            timeout_seconds=timeout_seconds,
            output_log_limit=output_log_limit,
            require_ready=require_ready,
            readiness_timeout_seconds=readiness_timeout_seconds,
            readiness_stable_samples=readiness_stable_samples,
            readiness_poll_interval_seconds=readiness_poll_interval_seconds,
            readiness_settle_seconds=readiness_settle_seconds,
        )

    @mcp.tool()
    def get_failstate_context(ctx: Context, profile_name: str = "failstate") -> Dict[str, Any]:
        """Return the active Failstate observability profile without mutating Unreal state."""
        return build_failstate_context(profile_name)

    logger.info("Observability tools registered successfully")
