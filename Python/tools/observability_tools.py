"""Read-mostly observability tools for UEMCP."""

from __future__ import annotations

import logging
from typing import Any, Callable, Dict, Optional

from mcp.server.fastmcp import Context, FastMCP

from uemcp_observability import (
    build_success_envelope,
    execute_bridge_command,
    get_failstate_context_data,
    server_metadata,
    utc_now,
)

logger = logging.getLogger("UnrealMCP")


def _default_connection_factory():
    from unreal_mcp_server import get_unreal_connection

    return get_unreal_connection()


def _editor_identity(data: Dict[str, Any]) -> Dict[str, Any]:
    return {
        key: data[key]
        for key in ("engine_version", "project_path", "plugin_version", "current_map")
        if key in data
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
    def get_failstate_context(ctx: Context, profile_name: str = "failstate") -> Dict[str, Any]:
        """Return the active Failstate observability profile without mutating Unreal state."""
        return build_failstate_context(profile_name)

    logger.info("Observability tools registered successfully")
