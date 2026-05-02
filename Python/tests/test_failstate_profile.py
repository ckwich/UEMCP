import json

from uemcp_observability import get_failstate_context_data, load_profile


def test_packaged_failstate_profile_is_shareable_default():
    profile = load_profile("failstate", include_local=False)

    assert profile["name"] == "failstate"
    assert profile["project_path"] == "C:/Path/To/Failstate"
    assert profile["preferred_worktree_path"] == "C:/Path/To/Failstate/.worktrees/phase1-combat-shell"
    assert profile["engine_version"] == "5.7"
    assert "Content/Failstate/Blueprints/Blockout" in profile["content_roots"]
    assert "Failstate.Phase1" in profile["automation_test_prefixes"]
    assert "LogTemp" in profile["log_categories"]


def test_load_profile_prefers_environment_profile_dir(monkeypatch, tmp_path):
    profile_dir = tmp_path / "profiles"
    profile_dir.mkdir()
    profile_path = profile_dir / "failstate.json"
    profile_path.write_text(
        json.dumps(
            {
                "name": "failstate",
                "project_path": "E:/Games/Failstate",
                "preferred_worktree_path": "E:/Games/Failstate/.worktrees/local",
                "engine_version": "5.7",
                "content_roots": ["Content/Failstate"],
                "automation_test_prefixes": ["Failstate.Local"],
                "log_categories": ["LogFailstate"],
                "known_maps": ["/Game/Failstate/Maps/Local"],
                "notes": ["local override"],
            }
        ),
        encoding="utf-8",
    )
    monkeypatch.setenv("UEMCP_PROFILE_DIR", str(profile_dir))

    profile = load_profile("failstate")

    assert profile["project_path"] == "E:/Games/Failstate"
    assert profile["automation_test_prefixes"] == ["Failstate.Local"]


def test_failstate_context_reports_profile_source_from_environment(monkeypatch, tmp_path):
    profile_dir = tmp_path / "profiles"
    project_path = tmp_path / "Failstate"
    worktree_path = project_path / ".worktrees" / "phase1-combat-shell"
    profile_dir.mkdir()
    worktree_path.mkdir(parents=True)
    (profile_dir / "failstate.json").write_text(
        json.dumps(
            {
                "name": "failstate",
                "project_path": str(project_path).replace("\\", "/"),
                "preferred_worktree_path": str(worktree_path).replace("\\", "/"),
                "engine_version": "5.7",
                "content_roots": ["Content/Failstate"],
                "automation_test_prefixes": ["Failstate.Phase1"],
                "log_categories": ["LogTemp"],
                "known_maps": [],
                "notes": [],
            }
        ),
        encoding="utf-8",
    )
    monkeypatch.setenv("UEMCP_PROFILE_DIR", str(profile_dir))

    context = get_failstate_context_data("failstate")

    assert context["active_profile"] == "failstate"
    assert context["profile"]["project_path"] == str(project_path).replace("\\", "/")
    assert context["profile_source"]["kind"] == "environment"
    assert context["profile_source"]["path"].endswith("failstate.json")
    assert context["read_only"] is True
    assert context["warnings"] == []
