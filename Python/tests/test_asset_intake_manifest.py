import json

import pytest


def _asset(package_name, *, asset_class="StaticMesh", dependencies=None):
    return {
        "asset_name": package_name.rsplit("/", 1)[-1],
        "object_path": f"{package_name}.{package_name.rsplit('/', 1)[-1]}",
        "package_name": package_name,
        "package_path": package_name.rsplit("/", 1)[0],
        "asset_class": asset_class,
        "asset_class_path": f"/Script/Engine.{asset_class}",
        "dependencies": dependencies or [],
        "referencers": [],
    }


def test_diff_snapshots_reports_added_removed_changed_and_suppresses_unchanged():
    from uemcp_asset_intake import diff_snapshots

    before = {
        "snapshot_id": "before",
        "assets": [
            _asset("/Game/Imported/SM_Removed"),
            _asset("/Game/Imported/SM_Changed", dependencies=["/Game/Materials/M_Old"]),
            _asset("/Game/Imported/SM_Unchanged"),
        ],
    }
    after = {
        "snapshot_id": "after",
        "assets": [
            _asset("/Game/Imported/SM_Changed", dependencies=["/Game/Materials/M_New"]),
            _asset("/Game/Imported/SM_Unchanged"),
            _asset("/Game/Imported/SM_Added"),
        ],
    }

    diff = diff_snapshots(before, after)

    assert [asset["package_name"] for asset in diff["added"]] == ["/Game/Imported/SM_Added"]
    assert [asset["package_name"] for asset in diff["removed"]] == ["/Game/Imported/SM_Removed"]
    assert [item["package_name"] for item in diff["changed"]] == ["/Game/Imported/SM_Changed"]
    assert diff["changed"][0]["changed_fields"] == ["dependencies"]
    assert "unchanged" not in diff
    assert diff["summary"] == {
        "added": 1,
        "removed": 1,
        "changed": 1,
        "unchanged": 1,
    }


def test_diff_snapshots_can_include_unchanged_assets():
    from uemcp_asset_intake import diff_snapshots

    before = {"snapshot_id": "before", "assets": [_asset("/Game/Imported/SM_Stable")]}
    after = {"snapshot_id": "after", "assets": [_asset("/Game/Imported/SM_Stable")]}

    diff = diff_snapshots(before, after, include_unchanged=True)

    assert [asset["package_name"] for asset in diff["unchanged"]] == [
        "/Game/Imported/SM_Stable"
    ]


def test_write_manifest_records_metadata_under_project_asset_intake_dir(tmp_path):
    from uemcp_asset_intake import write_manifest

    diff = {
        "before_snapshot_id": "before",
        "after_snapshot_id": "after",
        "added": [_asset("/Game/Imported/SM_Added")],
        "removed": [],
        "changed": [],
        "summary": {"added": 1, "removed": 0, "changed": 0, "unchanged": 0},
    }

    manifest = write_manifest(
        diff,
        "Tools/UEMCP/asset-intake/test-import.json",
        project_root=tmp_path,
        active_profile="asset-test",
        project_path="/Projects/Example/Example.uproject",
        notes=["Imported after a visible license acceptance prompt."],
        timestamp="2026-05-22T12:00:00Z",
    )

    manifest_path = tmp_path / "Tools" / "UEMCP" / "asset-intake" / "test-import.json"
    assert manifest["output_path"] == str(manifest_path)
    assert manifest_path.exists()
    written = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert written["active_profile"] == "asset-test"
    assert written["project_path"] == "/Projects/Example/Example.uproject"
    assert written["source_snapshot_ids"] == {"before": "before", "after": "after"}
    assert written["changed_package_names"] == ["/Game/Imported/SM_Added"]
    assert written["notes"] == ["Imported after a visible license acceptance prompt."]


def test_write_manifest_refuses_paths_outside_asset_intake_dir(tmp_path):
    from uemcp_asset_intake import write_manifest

    with pytest.raises(ValueError, match="Tools/UEMCP/asset-intake"):
        write_manifest(
            {"added": [], "removed": [], "changed": [], "summary": {}},
            "Saved/unsafe.json",
            project_root=tmp_path,
        )
