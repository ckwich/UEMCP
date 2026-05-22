"""Interactive asset workflow tools for UEMCP."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from mcp.server.fastmcp import FastMCP

from uemcp_asset_intake import diff_snapshots, write_manifest
from uemcp_observability import build_error_envelope, build_success_envelope, execute_bridge_command, utc_now

MAX_ASSET_INTAKE_SNAPSHOT_LIMIT = 10000
MAX_ASSET_OPERATION_LIMIT = 1000


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


def _is_game_path(value: str) -> bool:
    return str(value).strip().startswith("/Game/")


def _has_wildcard(value: str) -> bool:
    return "*" in value or "?" in value


def _error(tool: str, message: str, category: str) -> Dict[str, Any]:
    return build_error_envelope(
        tool=tool,
        started_at=utc_now(),
        message=message,
        category=category,
    )


def _validate_game_path(tool: str, value: str, field_name: str) -> Optional[Dict[str, Any]]:
    if not _is_game_path(value):
        return _error(
            tool,
            f"{field_name} must be an Unreal /Game/ package path",
            f"invalid_asset_{field_name}",
        )
    if _has_wildcard(value):
        return _error(
            tool,
            f"{field_name} must be exact and may not contain wildcards",
            f"invalid_asset_{field_name}",
        )
    return None


def _bridge_asset_command(
    command: str,
    connection_factory: Callable[[], Any],
    params: Dict[str, Any],
) -> Dict[str, Any]:
    return execute_bridge_command(
        tool=command,
        command=command,
        connection_factory=connection_factory,
        params=params,
    )


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


def build_asset_import_from_disk(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    source_files: List[str],
    destination_path: str,
    replace_existing: bool = False,
    save_imported_assets: bool = False,
    dry_run: bool = True,
) -> Dict[str, Any]:
    cleaned_sources = _clean_string_list(source_files)
    if not cleaned_sources:
        return _error(
            "asset_import_from_disk",
            "asset_import_from_disk requires at least one source file",
            "invalid_asset_source_files",
        )
    path_error = _validate_game_path("asset_import_from_disk", destination_path, "destination_path")
    if path_error:
        return path_error
    return _bridge_asset_command(
        "asset_import_from_disk",
        connection_factory,
        {
            "source_files": cleaned_sources,
            "destination_path": destination_path,
            "replace_existing": bool(replace_existing),
            "save_imported_assets": bool(save_imported_assets),
            "dry_run": bool(dry_run),
        },
    )


def build_asset_organize_plan(*, operations: List[Dict[str, Any]]) -> Dict[str, Any]:
    started_at = utc_now()
    try:
        if not operations:
            raise ValueError("asset_organize_plan requires at least one operation")
        normalized: list[dict[str, Any]] = []
        for operation in operations[:MAX_ASSET_OPERATION_LIMIT]:
            op = dict(operation)
            operation_name = str(op.get("operation") or "").strip()
            asset_path = str(op.get("asset_path") or "").strip()
            if operation_name not in {"rename", "move", "duplicate", "delete", "save", "fixup_redirectors"}:
                raise ValueError(f"Unsupported asset organization operation: {operation_name}")
            if operation_name != "fixup_redirectors":
                if not _is_game_path(asset_path) or _has_wildcard(asset_path):
                    raise ValueError("Asset organization operations require exact /Game/ asset_path values")
            destination_path = str(op.get("destination_path") or "").strip()
            if operation_name in {"move", "duplicate"}:
                if not _is_game_path(destination_path) or _has_wildcard(destination_path):
                    raise ValueError("move/duplicate operations require exact /Game/ destination_path values")
            if operation_name == "rename" and not str(op.get("new_name") or "").strip():
                raise ValueError("rename operations require new_name")
            normalized.append(op)
        return build_success_envelope(
            tool="asset_organize_plan",
            started_at=started_at,
            data={"dry_run": True, "operation_count": len(normalized), "operations": normalized},
        )
    except Exception as exc:
        return build_error_envelope(
            tool="asset_organize_plan",
            started_at=started_at,
            message=str(exc),
            category="invalid_asset_organize_plan",
        )


def build_asset_rename(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    asset_path: str,
    new_name: str,
    dry_run: bool = True,
) -> Dict[str, Any]:
    path_error = _validate_game_path("asset_rename", asset_path, "asset_path")
    if path_error:
        return path_error
    if not str(new_name).strip() or "/" in new_name or "\\" in new_name:
        return _error("asset_rename", "new_name must be a leaf asset name", "invalid_asset_new_name")
    return _bridge_asset_command(
        "asset_rename",
        connection_factory,
        {"asset_path": asset_path, "new_name": str(new_name).strip(), "dry_run": bool(dry_run)},
    )


def build_asset_move(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    asset_path: str,
    destination_path: str,
    dry_run: bool = True,
) -> Dict[str, Any]:
    for field_name, value in {"asset_path": asset_path, "destination_path": destination_path}.items():
        path_error = _validate_game_path("asset_move", value, field_name)
        if path_error:
            return path_error
    return _bridge_asset_command(
        "asset_move",
        connection_factory,
        {"asset_path": asset_path, "destination_path": destination_path, "dry_run": bool(dry_run)},
    )


def build_asset_duplicate(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    asset_path: str,
    destination_path: str,
    dry_run: bool = True,
) -> Dict[str, Any]:
    for field_name, value in {"asset_path": asset_path, "destination_path": destination_path}.items():
        path_error = _validate_game_path("asset_duplicate", value, field_name)
        if path_error:
            return path_error
    return _bridge_asset_command(
        "asset_duplicate",
        connection_factory,
        {"asset_path": asset_path, "destination_path": destination_path, "dry_run": bool(dry_run)},
    )


def build_asset_delete(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    asset_path: str,
    dry_run: bool = True,
) -> Dict[str, Any]:
    path_error = _validate_game_path("asset_delete", asset_path, "asset_path")
    if path_error:
        return path_error
    return _bridge_asset_command(
        "asset_delete",
        connection_factory,
        {"asset_path": asset_path, "dry_run": bool(dry_run)},
    )


def build_asset_save_packages(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    package_paths: List[str],
    only_if_dirty: bool = True,
) -> Dict[str, Any]:
    cleaned_paths = _clean_string_list(package_paths)
    if not cleaned_paths:
        return _error("asset_save_packages", "package_paths is required", "invalid_asset_package_paths")
    for package_path in cleaned_paths:
        path_error = _validate_game_path("asset_save_packages", package_path, "package_paths")
        if path_error:
            return path_error
    return _bridge_asset_command(
        "asset_save_packages",
        connection_factory,
        {"package_paths": cleaned_paths, "only_if_dirty": bool(only_if_dirty)},
    )


def build_asset_fixup_redirectors(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    roots: List[str],
    dry_run: bool = True,
) -> Dict[str, Any]:
    cleaned_roots = _clean_string_list(roots)
    if not cleaned_roots:
        return _error("asset_fixup_redirectors", "roots is required", "invalid_asset_roots")
    for root in cleaned_roots:
        if not _is_game_path(root) and root != "/Game":
            return _error("asset_fixup_redirectors", "roots must be /Game package paths", "invalid_asset_roots")
    return _bridge_asset_command(
        "asset_fixup_redirectors",
        connection_factory,
        {"roots": cleaned_roots, "dry_run": bool(dry_run)},
    )


def build_asset_prepare_for_level(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    asset_path: str,
) -> Dict[str, Any]:
    path_error = _validate_game_path("asset_prepare_for_level", asset_path, "asset_path")
    if path_error:
        return path_error
    return _bridge_asset_command("asset_prepare_for_level", connection_factory, {"asset_path": asset_path})


def build_asset_create_blueprint_wrapper(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    asset_path: str,
    target_package_path: str,
    component_name: str = "AssetMesh",
    parent_class: str = "Actor",
    dry_run: bool = True,
    save_asset: bool = False,
) -> Dict[str, Any]:
    for field_name, value in {"asset_path": asset_path, "target_package_path": target_package_path}.items():
        path_error = _validate_game_path("asset_create_blueprint_wrapper", value, field_name)
        if path_error:
            return path_error
    return _bridge_asset_command(
        "asset_create_blueprint_wrapper",
        connection_factory,
        {
            "asset_path": asset_path,
            "target_package_path": target_package_path,
            "component_name": str(component_name or "AssetMesh"),
            "parent_class": str(parent_class or "Actor"),
            "dry_run": bool(dry_run),
            "save_asset": bool(save_asset),
        },
    )


def _normalize_vector(value: Any, default: list[float]) -> list[float]:
    if not isinstance(value, list) or len(value) != 3:
        return list(default)
    return [float(value[0]), float(value[1]), float(value[2])]


def build_asset_place_in_level_plan(
    *,
    placements: List[Dict[str, Any]],
    target_map: Optional[str] = None,
) -> Dict[str, Any]:
    started_at = utc_now()
    try:
        if not placements:
            raise ValueError("placements is required")
        normalized: list[dict[str, Any]] = []
        for index, placement in enumerate(placements[:MAX_ASSET_OPERATION_LIMIT], 1):
            item = dict(placement)
            asset_path = str(item.get("asset_path") or "").strip()
            if not _is_game_path(asset_path) or _has_wildcard(asset_path):
                raise ValueError("placements require exact /Game/ asset_path values")
            actor_name = str(item.get("actor_name") or f"PlacedAsset_{index}").strip()
            normalized.append(
                {
                    "asset_path": asset_path,
                    "actor_name": actor_name,
                    "location": _normalize_vector(item.get("location"), [0, 0, 0]),
                    "rotation": _normalize_vector(item.get("rotation"), [0, 0, 0]),
                    "scale": _normalize_vector(item.get("scale"), [1, 1, 1]),
                }
            )
        return build_success_envelope(
            tool="asset_place_in_level_plan",
            started_at=started_at,
            data={
                "dry_run": True,
                "target_map": target_map,
                "placement_count": len(normalized),
                "placements": normalized,
            },
        )
    except Exception as exc:
        return build_error_envelope(
            tool="asset_place_in_level_plan",
            started_at=started_at,
            message=str(exc),
            category="invalid_asset_placement_plan",
        )


def build_asset_place_in_level(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    placements: List[Dict[str, Any]],
    target_map: Optional[str] = None,
    dry_run: bool = True,
    save_level: bool = False,
) -> Dict[str, Any]:
    plan = build_asset_place_in_level_plan(placements=placements, target_map=target_map)
    if not plan.get("ok"):
        return plan
    return _bridge_asset_command(
        "asset_place_in_level",
        connection_factory,
        {
            "placements": plan["data"]["placements"],
            "target_map": target_map,
            "dry_run": bool(dry_run),
            "save_level": bool(save_level),
        },
    )


def build_asset_validate_level_placements(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    expected_actors: List[str],
    target_map: Optional[str] = None,
) -> Dict[str, Any]:
    cleaned_actors = _clean_string_list(expected_actors)
    if not cleaned_actors:
        return _error(
            "asset_validate_level_placements",
            "expected_actors is required",
            "invalid_asset_expected_actors",
        )
    return _bridge_asset_command(
        "asset_validate_level_placements",
        connection_factory,
        {"expected_actors": cleaned_actors, "target_map": target_map},
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

    @mcp.tool()
    def asset_import_from_disk(
        source_files: List[str],
        destination_path: str,
        replace_existing: bool = False,
        save_imported_assets: bool = False,
        dry_run: bool = True,
    ) -> Dict[str, Any]:
        """Import files from disk through Unreal Editor asset import tasks."""
        return build_asset_import_from_disk(
            source_files=source_files,
            destination_path=destination_path,
            replace_existing=replace_existing,
            save_imported_assets=save_imported_assets,
            dry_run=dry_run,
        )

    @mcp.tool()
    def asset_organize_plan(operations: List[Dict[str, Any]]) -> Dict[str, Any]:
        """Validate a dry-run asset organization operation list."""
        return build_asset_organize_plan(operations=operations)

    @mcp.tool()
    def asset_rename(
        asset_path: str,
        new_name: str,
        dry_run: bool = True,
    ) -> Dict[str, Any]:
        """Rename one exact Unreal asset package through editor-backed APIs."""
        return build_asset_rename(asset_path=asset_path, new_name=new_name, dry_run=dry_run)

    @mcp.tool()
    def asset_move(
        asset_path: str,
        destination_path: str,
        dry_run: bool = True,
    ) -> Dict[str, Any]:
        """Move one exact Unreal asset package through editor-backed APIs."""
        return build_asset_move(
            asset_path=asset_path,
            destination_path=destination_path,
            dry_run=dry_run,
        )

    @mcp.tool()
    def asset_duplicate(
        asset_path: str,
        destination_path: str,
        dry_run: bool = True,
    ) -> Dict[str, Any]:
        """Duplicate one exact Unreal asset package through editor-backed APIs."""
        return build_asset_duplicate(
            asset_path=asset_path,
            destination_path=destination_path,
            dry_run=dry_run,
        )

    @mcp.tool()
    def asset_delete(asset_path: str, dry_run: bool = True) -> Dict[str, Any]:
        """Delete one exact Unreal asset package through editor-backed APIs."""
        return build_asset_delete(asset_path=asset_path, dry_run=dry_run)

    @mcp.tool()
    def asset_save_packages(
        package_paths: List[str],
        only_if_dirty: bool = True,
    ) -> Dict[str, Any]:
        """Save exact Unreal asset packages through editor-backed APIs."""
        return build_asset_save_packages(
            package_paths=package_paths,
            only_if_dirty=only_if_dirty,
        )

    @mcp.tool()
    def asset_fixup_redirectors(roots: List[str], dry_run: bool = True) -> Dict[str, Any]:
        """Find or fix redirectors under exact content roots."""
        return build_asset_fixup_redirectors(roots=roots, dry_run=dry_run)

    @mcp.tool()
    def asset_prepare_for_level(asset_path: str) -> Dict[str, Any]:
        """Inspect whether an asset is ready for level placement."""
        return build_asset_prepare_for_level(asset_path=asset_path)

    @mcp.tool()
    def asset_create_blueprint_wrapper(
        asset_path: str,
        target_package_path: str,
        component_name: str = "AssetMesh",
        parent_class: str = "Actor",
        dry_run: bool = True,
        save_asset: bool = False,
    ) -> Dict[str, Any]:
        """Create or dry-run a placement-ready Blueprint wrapper for an asset."""
        return build_asset_create_blueprint_wrapper(
            asset_path=asset_path,
            target_package_path=target_package_path,
            component_name=component_name,
            parent_class=parent_class,
            dry_run=dry_run,
            save_asset=save_asset,
        )

    @mcp.tool()
    def asset_place_in_level_plan(
        placements: List[Dict[str, Any]],
        target_map: Optional[str] = None,
    ) -> Dict[str, Any]:
        """Validate a dry-run level placement plan."""
        return build_asset_place_in_level_plan(placements=placements, target_map=target_map)

    @mcp.tool()
    def asset_place_in_level(
        placements: List[Dict[str, Any]],
        target_map: Optional[str] = None,
        dry_run: bool = True,
        save_level: bool = False,
    ) -> Dict[str, Any]:
        """Place prepared assets into the current editor level."""
        return build_asset_place_in_level(
            placements=placements,
            target_map=target_map,
            dry_run=dry_run,
            save_level=save_level,
        )

    @mcp.tool()
    def asset_validate_level_placements(
        expected_actors: List[str],
        target_map: Optional[str] = None,
    ) -> Dict[str, Any]:
        """Validate expected placed actors in the current editor level."""
        return build_asset_validate_level_placements(
            expected_actors=expected_actors,
            target_map=target_map,
        )
