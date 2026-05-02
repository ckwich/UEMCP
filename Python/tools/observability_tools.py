"""Read-mostly observability tools for UEMCP."""

from __future__ import annotations

import logging
from typing import Any, Callable, Dict, List, Optional

from mcp.server.fastmcp import Context, FastMCP

from uemcp_observability import (
    build_error_envelope,
    build_success_envelope,
    execute_bridge_command,
    get_failstate_context_data,
    server_metadata,
    utc_now,
)

logger = logging.getLogger("UnrealMCP")

MAX_PROFILE_AUTOMATION_RUNS = 50
MAX_EVENT_SNIPPETS = 5


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


def build_profile_automation_run(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    profile_name: str = "failstate",
    test_name: Optional[str] = None,
    prefix: Optional[str] = None,
    limit: int = 10,
    timeout_seconds: float = 30.0,
    output_log_limit: int = 25,
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
        return build_error_envelope(
            tool="run_profile_automation_tests",
            started_at=started_at,
            message=f"Profile '{profile_name}' does not define an automation test prefix",
            category="invalid_params",
            warnings=warnings,
        )

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
            return build_error_envelope(
                tool="run_profile_automation_tests",
                started_at=started_at,
                message=f"Failed to discover automation tests for prefix '{resolved_prefix}'",
                category="automation_failed",
                raw=discovery_envelope.get("error"),
                warnings=warnings,
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
        "summary": _automation_summary(results),
        "tests": results,
        "discovery": discovery,
        "output_log_tail": output_log_tail,
        "evidence_refs": {
            "discovery_request_id": discovery["request_id"] if discovery else None,
            "run_request_ids": run_request_ids,
            "output_log_request_id": output_log_request_id,
        },
    }

    return build_success_envelope(
        tool="run_profile_automation_tests",
        started_at=started_at,
        data=data,
        warnings=warnings,
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
    ) -> Dict[str, Any]:
        """Run one automation test or a bounded profile prefix batch with a compact summary."""
        return build_profile_automation_run(
            profile_name=profile_name,
            test_name=test_name,
            prefix=prefix,
            limit=limit,
            timeout_seconds=timeout_seconds,
            output_log_limit=output_log_limit,
        )

    @mcp.tool()
    def get_failstate_context(ctx: Context, profile_name: str = "failstate") -> Dict[str, Any]:
        """Return the active Failstate observability profile without mutating Unreal state."""
        return build_failstate_context(profile_name)

    logger.info("Observability tools registered successfully")
