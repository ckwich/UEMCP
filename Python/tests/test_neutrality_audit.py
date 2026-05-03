from pathlib import Path

from uemcp_neutrality import NeutralityFinding, is_allowed, scan_paths


def test_neutrality_audit_reports_project_specific_strings_outside_allowlist(tmp_path):
    config = {
        "patterns": ["Failstate", "Content/Failstate"],
        "allowed_occurrences": [
            {"path": "Docs/allowed.md", "reason": "documented compatibility example"}
        ],
    }
    allowed = tmp_path / "Docs" / "allowed.md"
    blocked = tmp_path / "Python" / "tools" / "new_tool.py"
    allowed.parent.mkdir(parents=True)
    blocked.parent.mkdir(parents=True)
    allowed.write_text("Failstate example is allowed here\n", encoding="utf-8")
    blocked.write_text(
        "ROOT = 'Content/Failstate/Blueprints'\n",
        encoding="utf-8",
    )

    findings = scan_paths(tmp_path, [allowed, blocked], config)

    assert findings == [
        NeutralityFinding(
            path="Python/tools/new_tool.py",
            line=1,
            pattern="Content/Failstate",
            text="ROOT = 'Content/Failstate/Blueprints'",
        )
    ]


def test_neutrality_audit_can_allow_specific_patterns_only():
    config = {
        "patterns": ["Failstate", "Content/Failstate"],
        "allowed_occurrences": [
            {
                "path": "Scripts/example.ps1",
                "patterns": ["Failstate"],
                "reason": "project name is allowed, paths are not",
            }
        ],
    }

    assert is_allowed(config, path="Scripts/example.ps1", pattern="Failstate") is True
    assert is_allowed(config, path="Scripts/example.ps1", pattern="Content/Failstate") is False
    assert is_allowed(config, path="Python/tools/example.py", pattern="Failstate") is False


def test_neutrality_audit_skips_non_text_suffixes(tmp_path):
    config = {"patterns": ["Failstate"], "allowed_occurrences": []}
    binary_like = tmp_path / "Content" / "Example.uasset"
    binary_like.parent.mkdir(parents=True)
    binary_like.write_bytes(b"Failstate")

    assert scan_paths(tmp_path, [Path(binary_like)], config) == []
