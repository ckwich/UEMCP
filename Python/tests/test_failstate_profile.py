from uemcp_observability import get_failstate_context_data, load_profile


def test_load_failstate_profile_contains_observability_defaults():
    profile = load_profile("failstate")

    assert profile["name"] == "failstate"
    assert profile["project_path"] == "C:/Dev/Failstate"
    assert profile["preferred_worktree_path"] == "C:/Dev/Failstate/.worktrees/phase1-combat-shell"
    assert profile["engine_version"] == "5.7"
    assert "Content/Failstate/Blueprints/Blockout" in profile["content_roots"]
    assert "Failstate.Phase1" in profile["automation_test_prefixes"]
    assert "LogTemp" in profile["log_categories"]


def test_failstate_context_is_profile_data_without_editor_mutation():
    context = get_failstate_context_data("failstate")

    assert context["active_profile"] == "failstate"
    assert context["profile"]["project_path"] == "C:/Dev/Failstate"
    assert context["profile"]["preferred_worktree_path"].endswith("phase1-combat-shell")
    assert context["read_only"] is True
    assert isinstance(context["warnings"], list)
