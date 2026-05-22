"""Pure helpers for UEMCP asset intake snapshots, diffs, and manifests."""

from __future__ import annotations

import json
from copy import deepcopy
from pathlib import Path
from typing import Any, Mapping

from uemcp_observability import format_timestamp, utc_now

ASSET_INTAKE_RELATIVE_DIR = Path("Tools") / "UEMCP" / "asset-intake"
COMPARE_FIELDS = (
    "asset_name",
    "object_path",
    "package_path",
    "asset_class",
    "asset_class_path",
    "dependencies",
    "referencers",
    "tags",
)


def _stable_value(value: Any) -> str:
    return json.dumps(value, sort_keys=True, default=str, separators=(",", ":"))


def _asset_key(asset: Mapping[str, Any]) -> str:
    package_name = str(asset.get("package_name") or "").strip()
    object_path = str(asset.get("object_path") or "").strip()
    key = package_name or object_path
    if not key:
        raise ValueError(f"Asset snapshot entry is missing package_name/object_path: {asset!r}")
    return key


def normalize_snapshot(snapshot: Mapping[str, Any]) -> dict[str, Any]:
    """Return a deterministic snapshot copy keyed by package/object identity."""
    if not isinstance(snapshot, Mapping):
        raise ValueError("Asset snapshot must be an object")

    raw_assets = snapshot.get("assets")
    if not isinstance(raw_assets, list):
        raise ValueError("Asset snapshot must contain an assets list")

    assets: list[dict[str, Any]] = []
    seen: set[str] = set()
    for raw_asset in raw_assets:
        if not isinstance(raw_asset, Mapping):
            raise ValueError(f"Asset snapshot entry must be an object: {raw_asset!r}")
        asset = deepcopy(dict(raw_asset))
        key = _asset_key(asset)
        if key in seen:
            raise ValueError(f"Asset snapshot contains duplicate asset identity: {key}")
        seen.add(key)
        assets.append(asset)

    assets.sort(key=_asset_key)
    normalized = deepcopy(dict(snapshot))
    normalized["assets"] = assets
    return normalized


def _index_assets(snapshot: Mapping[str, Any]) -> dict[str, dict[str, Any]]:
    normalized = normalize_snapshot(snapshot)
    return {_asset_key(asset): asset for asset in normalized["assets"]}


def _changed_fields(before: Mapping[str, Any], after: Mapping[str, Any]) -> list[str]:
    changed: list[str] = []
    for field_name in COMPARE_FIELDS:
        if _stable_value(before.get(field_name)) != _stable_value(after.get(field_name)):
            changed.append(field_name)
    return changed


def diff_snapshots(
    before: Mapping[str, Any],
    after: Mapping[str, Any],
    *,
    include_unchanged: bool = False,
) -> dict[str, Any]:
    """Compare two asset intake snapshots by package/object identity."""
    before_assets = _index_assets(before)
    after_assets = _index_assets(after)

    before_keys = set(before_assets)
    after_keys = set(after_assets)

    added = [after_assets[key] for key in sorted(after_keys - before_keys)]
    removed = [before_assets[key] for key in sorted(before_keys - after_keys)]

    changed: list[dict[str, Any]] = []
    unchanged: list[dict[str, Any]] = []
    for key in sorted(before_keys & after_keys):
        fields = _changed_fields(before_assets[key], after_assets[key])
        if fields:
            changed.append(
                {
                    "package_name": str(after_assets[key].get("package_name") or key),
                    "changed_fields": fields,
                    "before": before_assets[key],
                    "after": after_assets[key],
                }
            )
        elif include_unchanged:
            unchanged.append(after_assets[key])

    result: dict[str, Any] = {
        "before_snapshot_id": before.get("snapshot_id"),
        "after_snapshot_id": after.get("snapshot_id"),
        "added": added,
        "removed": removed,
        "changed": changed,
        "summary": {
            "added": len(added),
            "removed": len(removed),
            "changed": len(changed),
            "unchanged": len(before_keys & after_keys) - len(changed),
        },
    }
    if include_unchanged:
        result["unchanged"] = unchanged
    return result


def _contains_asset_intake_parts(path: Path) -> bool:
    parts = path.parts
    needle = ASSET_INTAKE_RELATIVE_DIR.parts
    return any(parts[index : index + len(needle)] == needle for index in range(len(parts)))


def _resolve_manifest_path(
    output_path: str | Path,
    *,
    project_root: str | Path | None = None,
    allow_custom_output_path: bool = False,
) -> Path:
    raw_path = Path(output_path)
    base = Path(project_root) if project_root is not None else Path.cwd()
    resolved = raw_path if raw_path.is_absolute() else base / raw_path
    resolved = resolved.expanduser().resolve()

    if allow_custom_output_path:
        return resolved

    if project_root is not None:
        allowed_root = (Path(project_root) / ASSET_INTAKE_RELATIVE_DIR).expanduser().resolve()
        try:
            resolved.relative_to(allowed_root)
            return resolved
        except ValueError as exc:
            raise ValueError(
                "Asset intake manifests must be written under Tools/UEMCP/asset-intake"
            ) from exc

    if not _contains_asset_intake_parts(resolved):
        raise ValueError("Asset intake manifests must be written under Tools/UEMCP/asset-intake")
    return resolved


def _package_name_from_asset(asset: Mapping[str, Any]) -> str:
    return str(asset.get("package_name") or asset.get("object_path") or "").strip()


def changed_package_names(diff: Mapping[str, Any]) -> list[str]:
    names: set[str] = set()
    for field_name in ("added", "removed"):
        for asset in diff.get(field_name) or []:
            if isinstance(asset, Mapping):
                package_name = _package_name_from_asset(asset)
                if package_name:
                    names.add(package_name)
    for item in diff.get("changed") or []:
        if isinstance(item, Mapping):
            package_name = str(item.get("package_name") or "").strip()
            if package_name:
                names.add(package_name)
    return sorted(names)


def write_manifest(
    diff: Mapping[str, Any],
    output_path: str | Path,
    *,
    project_root: str | Path | None = None,
    active_profile: str | None = None,
    project_path: str | None = None,
    notes: list[str] | None = None,
    timestamp: str | None = None,
    allow_custom_output_path: bool = False,
) -> dict[str, Any]:
    """Write a project-owned JSON asset intake manifest."""
    if not isinstance(diff, Mapping):
        raise ValueError("diff must be an object")

    resolved_path = _resolve_manifest_path(
        output_path,
        project_root=project_root,
        allow_custom_output_path=allow_custom_output_path,
    )
    manifest = {
        "schema_version": 1,
        "written_at": timestamp or format_timestamp(utc_now()),
        "active_profile": active_profile,
        "project_path": project_path,
        "source_snapshot_ids": {
            "before": diff.get("before_snapshot_id"),
            "after": diff.get("after_snapshot_id"),
        },
        "changed_package_names": changed_package_names(diff),
        "summary": dict(diff.get("summary") or {}),
        "diff": deepcopy(dict(diff)),
        "notes": list(notes or []),
        "output_path": str(resolved_path),
    }

    resolved_path.parent.mkdir(parents=True, exist_ok=True)
    resolved_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest
