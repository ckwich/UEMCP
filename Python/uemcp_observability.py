"""Observability contract helpers for UEMCP."""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Dict, List, Mapping, Optional
from uuid import uuid4

UEMCP_SERVER_NAME = "UEMCP"
UEMCP_VERSION = "0.1.0"
DEFAULT_PROFILE = "failstate"


def utc_now() -> datetime:
    """Return an aware UTC timestamp."""
    return datetime.now(timezone.utc)


def format_timestamp(value: datetime) -> str:
    """Format a UTC timestamp with a stable Z suffix."""
    return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def make_request_id(tool: str) -> str:
    return f"{tool}-{uuid4().hex}"


def classify_error(message: str) -> str:
    lower_message = message.lower()
    if "timeout" in lower_message or "timed out" in lower_message:
        return "timeout"
    if "connect" in lower_message or "connection" in lower_message:
        return "connection_failed"
    if "unknown command" in lower_message or "unsupported" in lower_message:
        return "unsupported_command"
    if "missing" in lower_message or "invalid" in lower_message:
        return "invalid_params"
    if "slow task" in lower_message or "play in editor" in lower_message:
        return "editor_busy"
    if "not found" in lower_message and "asset" in lower_message:
        return "asset_not_found"
    if "automation" in lower_message and "fail" in lower_message:
        return "automation_failed"
    return "internal_error"


def build_success_envelope(
    *,
    tool: str,
    data: Any,
    started_at: Optional[datetime] = None,
    request_id: Optional[str] = None,
    warnings: Optional[List[str]] = None,
    editor: Optional[Mapping[str, Any]] = None,
) -> Dict[str, Any]:
    start = started_at or utc_now()
    finished_at = utc_now()
    return {
        "ok": True,
        "tool": tool,
        "request_id": request_id or make_request_id(tool),
        "started_at": format_timestamp(start),
        "finished_at": format_timestamp(finished_at),
        "duration_ms": max(0, int((finished_at - start).total_seconds() * 1000)),
        "editor": dict(editor or {}),
        "data": data,
        "warnings": warnings or [],
        "error": None,
    }


def build_error_envelope(
    *,
    tool: str,
    message: str,
    started_at: Optional[datetime] = None,
    request_id: Optional[str] = None,
    category: Optional[str] = None,
    raw: Any = None,
    warnings: Optional[List[str]] = None,
    editor: Optional[Mapping[str, Any]] = None,
) -> Dict[str, Any]:
    start = started_at or utc_now()
    finished_at = utc_now()
    return {
        "ok": False,
        "tool": tool,
        "request_id": request_id or make_request_id(tool),
        "started_at": format_timestamp(start),
        "finished_at": format_timestamp(finished_at),
        "duration_ms": max(0, int((finished_at - start).total_seconds() * 1000)),
        "editor": dict(editor or {}),
        "data": None,
        "warnings": warnings or [],
        "error": {
            "category": category or classify_error(message),
            "message": message,
            "raw": raw,
        },
    }


def unwrap_unreal_response(response: Optional[Mapping[str, Any]]) -> Dict[str, Any]:
    if not response:
        raise ValueError("No response from Unreal Engine")

    if response.get("status") == "error" or response.get("success") is False:
        message = response.get("error") or response.get("message") or "Unknown Unreal error"
        raise ValueError(str(message))

    if response.get("status") == "success" and isinstance(response.get("result"), dict):
        return dict(response["result"])

    return dict(response)


def server_metadata() -> Dict[str, str]:
    return {
        "name": UEMCP_SERVER_NAME,
        "version": UEMCP_VERSION,
    }


def execute_bridge_command(
    *,
    tool: str,
    command: str,
    connection_factory: Callable[[], Any],
    params: Optional[Dict[str, Any]] = None,
    data_builder: Optional[Callable[[Dict[str, Any]], Any]] = None,
    editor_builder: Optional[Callable[[Dict[str, Any]], Mapping[str, Any]]] = None,
) -> Dict[str, Any]:
    started_at = utc_now()
    request_id = make_request_id(tool)

    try:
        connection = connection_factory()
        if connection is None:
            return build_error_envelope(
                tool=tool,
                started_at=started_at,
                request_id=request_id,
                message="Unable to connect to Unreal Editor bridge",
            )

        raw_response = connection.send_command(command, params or {})
        data = unwrap_unreal_response(raw_response)
        return build_success_envelope(
            tool=tool,
            started_at=started_at,
            request_id=request_id,
            data=data_builder(data) if data_builder else data,
            editor=editor_builder(data) if editor_builder else {},
        )
    except Exception as exc:
        return build_error_envelope(
            tool=tool,
            started_at=started_at,
            request_id=request_id,
            message=str(exc),
        )


def _profile_path(profile_name: str) -> Path:
    return Path(__file__).resolve().parent / "profiles" / f"{profile_name}.json"


def load_profile(profile_name: str = DEFAULT_PROFILE) -> Dict[str, Any]:
    path = _profile_path(profile_name)
    with path.open("r", encoding="utf-8") as profile_file:
        profile = json.load(profile_file)
    profile.setdefault("name", profile_name)
    return profile


def get_failstate_context_data(profile_name: str = DEFAULT_PROFILE) -> Dict[str, Any]:
    profile = load_profile(profile_name)
    warnings: List[str] = []

    project_path = Path(profile["project_path"])
    worktree_path = Path(profile["preferred_worktree_path"])
    if not project_path.exists():
        warnings.append(f"Configured project path does not exist: {profile['project_path']}")
    if not worktree_path.exists():
        warnings.append(
            f"Configured preferred worktree path does not exist: {profile['preferred_worktree_path']}"
        )

    return {
        "active_profile": profile_name,
        "profile": profile,
        "read_only": True,
        "warnings": warnings,
    }
