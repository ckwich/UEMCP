"""Editor-owned level and map construction workflow tools for UEMCP."""

from __future__ import annotations

from typing import Any, Callable, Dict, List, Optional

from mcp.server.fastmcp import FastMCP

from uemcp_observability import build_error_envelope, build_success_envelope, execute_bridge_command, utc_now

MAX_LEVEL_OPERATION_LIMIT = 1000
MAX_LEVEL_LIST_LIMIT = 10000

SUPPORTED_LEVEL_OPERATIONS = {
    "ensure_actor",
    "set_actor_transform",
    "set_actor_folder",
    "set_actor_label",
    "set_actor_tags",
    "delete_actor",
}


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


def _is_game_root(value: str) -> bool:
    text = str(value).strip()
    return text == "/Game" or text.startswith("/Game/")


def _has_wildcard(value: str) -> bool:
    return "*" in value or "?" in value


def _error(tool: str, message: str, category: str) -> Dict[str, Any]:
    return build_error_envelope(
        tool=tool,
        started_at=utc_now(),
        message=message,
        category=category,
    )


def _validate_level_package_path(tool: str, value: str, field_name: str) -> Optional[Dict[str, Any]]:
    if not _is_game_path(value):
        return _error(
            tool,
            f"{field_name} must be an exact Unreal /Game/ map package path",
            f"invalid_level_{field_name}",
        )
    if _has_wildcard(value):
        return _error(
            tool,
            f"{field_name} must be exact and may not contain wildcards",
            f"invalid_level_{field_name}",
        )
    return None


def _validate_optional_target_map(tool: str, target_map: Optional[str]) -> Optional[Dict[str, Any]]:
    if target_map is None or str(target_map).strip() == "":
        return None
    return _validate_level_package_path(tool, str(target_map).strip(), "target_map")


def _bridge_level_command(
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


def _normalize_vector(value: Any, default: list[float]) -> list[float]:
    if not isinstance(value, list) or len(value) != 3:
        return list(default)
    return [float(value[0]), float(value[1]), float(value[2])]


def _normalize_operation(raw_operation: Dict[str, Any]) -> dict[str, Any]:
    op = str(raw_operation.get("op") or "").strip()
    if op not in SUPPORTED_LEVEL_OPERATIONS:
        raise ValueError(f"Unsupported level construction operation: {op}")

    actor_name = str(raw_operation.get("actor_name") or "").strip()
    if not actor_name:
        raise ValueError("level construction operations require actor_name")
    if _has_wildcard(actor_name):
        raise ValueError("actor_name must be exact and may not contain wildcards")

    actor_class = str(raw_operation.get("actor_class") or "").strip() or None
    asset_path = str(raw_operation.get("asset_path") or "").strip() or None
    if asset_path and (not _is_game_path(asset_path) or _has_wildcard(asset_path)):
        raise ValueError("asset_path must be an exact /Game/ asset path")
    if op == "ensure_actor" and not (actor_class or asset_path):
        raise ValueError("ensure_actor requires actor_class or asset_path")

    confirm_delete = bool(raw_operation.get("confirm_delete", False))
    if op == "delete_actor" and not confirm_delete:
        raise ValueError("delete_actor requires confirm_delete=true")

    return {
        "op": op,
        "actor_name": actor_name,
        "actor_class": actor_class,
        "asset_path": asset_path,
        "label": str(raw_operation.get("label") or "").strip(),
        "folder_path": str(raw_operation.get("folder_path") or "").strip(),
        "location": _normalize_vector(raw_operation.get("location"), [0, 0, 0]),
        "rotation": _normalize_vector(raw_operation.get("rotation"), [0, 0, 0]),
        "scale": _normalize_vector(raw_operation.get("scale"), [1, 1, 1]),
        "tags": _clean_string_list(raw_operation.get("tags")),
        "confirm_delete": confirm_delete,
    }


def _normalize_expected_actor(raw_actor: Dict[str, Any]) -> dict[str, Any]:
    actor_name = str(raw_actor.get("actor_name") or "").strip()
    if not actor_name:
        raise ValueError("expected actors require actor_name")
    if _has_wildcard(actor_name):
        raise ValueError("expected actor_name must be exact and may not contain wildcards")
    return {
        "actor_name": actor_name,
        "class": str(raw_actor.get("class") or raw_actor.get("actor_class") or "").strip(),
        "label": str(raw_actor.get("label") or "").strip(),
        "folder_path": str(raw_actor.get("folder_path") or "").strip(),
        "tags": _clean_string_list(raw_actor.get("tags")),
        "location": _normalize_vector(raw_actor.get("location"), [0, 0, 0])
        if "location" in raw_actor
        else None,
    }


def build_level_list_maps(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    roots: List[str],
    limit: int = 500,
) -> Dict[str, Any]:
    cleaned_roots = _clean_string_list(roots)
    if not cleaned_roots:
        return _error("level_list_maps", "roots is required", "invalid_level_roots")
    for root in cleaned_roots:
        if not _is_game_root(root) or _has_wildcard(root):
            return _error("level_list_maps", "roots must be exact /Game paths", "invalid_level_roots")
    bounded_limit = max(1, min(int(limit), MAX_LEVEL_LIST_LIMIT))
    return _bridge_level_command(
        "level_list_maps",
        connection_factory,
        {"roots": cleaned_roots, "limit": bounded_limit},
    )


def build_level_create(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    package_path: str,
    template_path: Optional[str] = None,
    save_existing: bool = False,
    save_new_level: bool = True,
    fail_if_exists: bool = True,
    dry_run: bool = True,
) -> Dict[str, Any]:
    path_error = _validate_level_package_path("level_create", package_path, "package_path")
    if path_error:
        return path_error
    if template_path:
        template_error = _validate_level_package_path("level_create", template_path, "template_path")
        if template_error:
            return template_error
    return _bridge_level_command(
        "level_create",
        connection_factory,
        {
            "package_path": package_path,
            "template_path": template_path,
            "save_existing": bool(save_existing),
            "save_new_level": bool(save_new_level),
            "fail_if_exists": bool(fail_if_exists),
            "dry_run": bool(dry_run),
        },
    )


def build_level_open(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    package_path: str,
    save_existing: bool = False,
    require_exists: bool = True,
) -> Dict[str, Any]:
    path_error = _validate_level_package_path("level_open", package_path, "package_path")
    if path_error:
        return path_error
    return _bridge_level_command(
        "level_open",
        connection_factory,
        {
            "package_path": package_path,
            "save_existing": bool(save_existing),
            "require_exists": bool(require_exists),
        },
    )


def build_level_save(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    package_path: Optional[str] = None,
    only_if_dirty: bool = True,
    include_external_actor_packages: bool = True,
) -> Dict[str, Any]:
    if package_path:
        path_error = _validate_level_package_path("level_save", package_path, "package_path")
        if path_error:
            return path_error
    return _bridge_level_command(
        "level_save",
        connection_factory,
        {
            "package_path": package_path,
            "only_if_dirty": bool(only_if_dirty),
            "include_external_actor_packages": bool(include_external_actor_packages),
        },
    )


def build_level_construction_plan(
    *,
    operations: List[Dict[str, Any]],
    target_map: Optional[str] = None,
) -> Dict[str, Any]:
    started_at = utc_now()
    try:
        target_error = _validate_optional_target_map("level_construction_plan", target_map)
        if target_error:
            return target_error
        if not operations:
            raise ValueError("level_construction_plan requires at least one operation")
        normalized = [
            _normalize_operation(dict(operation))
            for operation in operations[:MAX_LEVEL_OPERATION_LIMIT]
        ]
        return build_success_envelope(
            tool="level_construction_plan",
            started_at=started_at,
            data={
                "dry_run": True,
                "target_map": target_map,
                "operation_count": len(normalized),
                "operations": normalized,
            },
        )
    except Exception as exc:
        return build_error_envelope(
            tool="level_construction_plan",
            started_at=started_at,
            message=str(exc),
            category="invalid_level_construction_plan",
        )


def build_level_apply_construction_plan(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    operations: List[Dict[str, Any]],
    target_map: Optional[str] = None,
    open_level: bool = True,
    create_if_missing: bool = False,
    save_level: bool = False,
    dry_run: bool = True,
) -> Dict[str, Any]:
    plan = build_level_construction_plan(operations=operations, target_map=target_map)
    if not plan.get("ok"):
        return plan
    return _bridge_level_command(
        "level_apply_construction_plan",
        connection_factory,
        {
            "target_map": target_map,
            "operations": plan["data"]["operations"],
            "open_level": bool(open_level),
            "create_if_missing": bool(create_if_missing),
            "save_level": bool(save_level),
            "dry_run": bool(dry_run),
        },
    )


def build_level_validate_construction(
    connection_factory: Callable[[], Any] = _default_connection_factory,
    *,
    expected_actors: List[Dict[str, Any]],
    target_map: Optional[str] = None,
    location_tolerance: float = 1.0,
) -> Dict[str, Any]:
    target_error = _validate_optional_target_map("level_validate_construction", target_map)
    if target_error:
        return target_error
    if not expected_actors:
        return _error(
            "level_validate_construction",
            "expected_actors is required",
            "invalid_level_expected_actors",
        )
    try:
        normalized = [_normalize_expected_actor(dict(actor)) for actor in expected_actors]
    except Exception as exc:
        return _error("level_validate_construction", str(exc), "invalid_level_expected_actors")
    return _bridge_level_command(
        "level_validate_construction",
        connection_factory,
        {
            "target_map": target_map,
            "expected_actors": normalized,
            "location_tolerance": float(location_tolerance),
        },
    )


def register_level_workflow_tools(mcp: FastMCP):
    """Register editor-owned level workflow tools."""

    @mcp.tool()
    def level_list_maps(roots: List[str], limit: int = 500) -> Dict[str, Any]:
        """List map packages under exact content roots."""
        return build_level_list_maps(roots=roots, limit=limit)

    @mcp.tool()
    def level_create(
        package_path: str,
        template_path: Optional[str] = None,
        save_existing: bool = False,
        save_new_level: bool = True,
        fail_if_exists: bool = True,
        dry_run: bool = True,
    ) -> Dict[str, Any]:
        """Create or dry-run a new editor level at an exact package path."""
        return build_level_create(
            package_path=package_path,
            template_path=template_path,
            save_existing=save_existing,
            save_new_level=save_new_level,
            fail_if_exists=fail_if_exists,
            dry_run=dry_run,
        )

    @mcp.tool()
    def level_open(
        package_path: str,
        save_existing: bool = False,
        require_exists: bool = True,
    ) -> Dict[str, Any]:
        """Open an exact map package through Unreal Editor loading APIs."""
        return build_level_open(
            package_path=package_path,
            save_existing=save_existing,
            require_exists=require_exists,
        )

    @mcp.tool()
    def level_save(
        package_path: Optional[str] = None,
        only_if_dirty: bool = True,
        include_external_actor_packages: bool = True,
    ) -> Dict[str, Any]:
        """Save the current editor map with structured package evidence."""
        return build_level_save(
            package_path=package_path,
            only_if_dirty=only_if_dirty,
            include_external_actor_packages=include_external_actor_packages,
        )

    @mcp.tool()
    def level_construction_plan(
        operations: List[Dict[str, Any]],
        target_map: Optional[str] = None,
    ) -> Dict[str, Any]:
        """Validate a declarative level construction plan without mutating Unreal."""
        return build_level_construction_plan(operations=operations, target_map=target_map)

    @mcp.tool()
    def level_apply_construction_plan(
        operations: List[Dict[str, Any]],
        target_map: Optional[str] = None,
        open_level: bool = True,
        create_if_missing: bool = False,
        save_level: bool = False,
        dry_run: bool = True,
    ) -> Dict[str, Any]:
        """Apply or dry-run a declarative level construction plan."""
        return build_level_apply_construction_plan(
            operations=operations,
            target_map=target_map,
            open_level=open_level,
            create_if_missing=create_if_missing,
            save_level=save_level,
            dry_run=dry_run,
        )

    @mcp.tool()
    def level_validate_construction(
        expected_actors: List[Dict[str, Any]],
        target_map: Optional[str] = None,
        location_tolerance: float = 1.0,
    ) -> Dict[str, Any]:
        """Validate expected actors in the current editor level."""
        return build_level_validate_construction(
            expected_actors=expected_actors,
            target_map=target_map,
            location_tolerance=location_tolerance,
        )
