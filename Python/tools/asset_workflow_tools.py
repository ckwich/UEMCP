"""Interactive asset workflow tools for UEMCP."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from mcp.server.fastmcp import FastMCP

from uemcp_asset_intake import diff_snapshots, write_manifest
from uemcp_observability import build_error_envelope, build_success_envelope, execute_bridge_command, utc_now

MAX_ASSET_INTAKE_SNAPSHOT_LIMIT = 10000


def _default_connection_factory():
    from unreal_mcp_server import get_unreal_connection

    return get_unreal_connection()


def _clean_string_list(values: Optional[List[str]]) -> list[str]:
    if not values:
        return []
    cleaned: list[str] = []
    for value in values:
        text = str(value).strip()
        if text:
            cleaned.append(text)
    return cleaned


def build_asset_intake_snapshot(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    roots: List[str],
    classes: Optional[List[str]] = None,
    include_dependencies: bool = True,
    include_referencers: bool = False,
    include_tags: bool = True,
    limit: int = 5000,
) -> Dict[str, Any]:
    """Capture read-only Asset Registry inventory for one or more content roots."""
    started_at = utc_now()
    cleaned_roots = _clean_string_list(roots)
    if not cleaned_roots:
        return build_error_envelope(
            tool="asset_intake_snapshot",
            started_at=started_at,
            message="asset_intake_snapshot requires at least one non-empty root",
            category="invalid_asset_intake_snapshot_roots",
        )

    bounded_limit = max(1, min(int(limit), MAX_ASSET_INTAKE_SNAPSHOT_LIMIT))
    params: Dict[str, Any] = {
        "roots": cleaned_roots,
        "include_dependencies": bool(include_dependencies),
        "include_referencers": bool(include_referencers),
        "include_tags": bool(include_tags),
        "limit": bounded_limit,
    }
    cleaned_classes = _clean_string_list(classes)
    if cleaned_classes:
        params["classes"] = cleaned_classes

    return execute_bridge_command(
        tool="asset_intake_snapshot",
        command="asset_intake_snapshot",
        connection_factory=connection_factory,
        params=params,
    )


def build_asset_intake_diff(
    *,
    before: Dict[str, Any],
    after: Dict[str, Any],
    include_unchanged: bool = False,
) -> Dict[str, Any]:
    """Compare two asset intake snapshots without contacting the editor."""
    started_at = utc_now()
    try:
        return build_success_envelope(
            tool="asset_intake_diff",
            started_at=started_at,
            data=diff_snapshots(before, after, include_unchanged=include_unchanged),
        )
    except Exception as exc:
        return build_error_envelope(
            tool="asset_intake_diff",
            started_at=started_at,
            message=str(exc),
            category="invalid_asset_intake_diff",
        )


def build_asset_intake_write_manifest(
    *,
    diff: Dict[str, Any],
    output_path: str,
    notes: Optional[List[str]] = None,
    project_root: Optional[str] = None,
    active_profile: Optional[str] = None,
    project_path: Optional[str] = None,
    allow_custom_output_path: bool = False,
) -> Dict[str, Any]:
    """Write a project-owned asset intake manifest without touching Unreal assets."""
    started_at = utc_now()
    try:
        return build_success_envelope(
            tool="asset_intake_write_manifest",
            started_at=started_at,
            data=write_manifest(
                diff,
                Path(output_path),
                notes=notes,
                project_root=project_root,
                active_profile=active_profile,
                project_path=project_path,
                allow_custom_output_path=allow_custom_output_path,
            ),
        )
    except Exception as exc:
        return build_error_envelope(
            tool="asset_intake_write_manifest",
            started_at=started_at,
            message=str(exc),
            category="invalid_asset_intake_manifest",
        )


def register_asset_workflow_tools(mcp: FastMCP):
    """Register interactive asset workflow tools."""

    @mcp.tool()
    def asset_intake_snapshot(
        roots: List[str],
        classes: Optional[List[str]] = None,
        include_dependencies: bool = True,
        include_referencers: bool = False,
        include_tags: bool = True,
        limit: int = 5000,
    ) -> Dict[str, Any]:
        """Capture read-only Asset Registry inventory for one or more content roots."""
        return build_asset_intake_snapshot(
            roots=roots,
            classes=classes,
            include_dependencies=include_dependencies,
            include_referencers=include_referencers,
            include_tags=include_tags,
            limit=limit,
        )

    @mcp.tool()
    def asset_intake_diff(
        before: Dict[str, Any],
        after: Dict[str, Any],
        include_unchanged: bool = False,
    ) -> Dict[str, Any]:
        """Compare two asset intake snapshots without contacting the editor."""
        return build_asset_intake_diff(
            before=before,
            after=after,
            include_unchanged=include_unchanged,
        )

    @mcp.tool()
    def asset_intake_write_manifest(
        diff: Dict[str, Any],
        output_path: str,
        notes: Optional[List[str]] = None,
        project_root: Optional[str] = None,
        active_profile: Optional[str] = None,
        project_path: Optional[str] = None,
        allow_custom_output_path: bool = False,
    ) -> Dict[str, Any]:
        """Write a project-owned JSON manifest for an asset intake diff."""
        return build_asset_intake_write_manifest(
            diff=diff,
            output_path=output_path,
            notes=notes,
            project_root=project_root,
            active_profile=active_profile,
            project_path=project_path,
            allow_custom_output_path=allow_custom_output_path,
        )
