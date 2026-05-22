from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def test_level_workflow_smoke_script_exercises_create_apply_validate_save_cleanup():
    script = (REPO_ROOT / "Scripts" / "Smoke-UEMCPLevelWorkflow.ps1").read_text(
        encoding="utf-8"
    )

    assert "level_create" in script
    assert "level_apply_construction_plan" in script
    assert "level_validate_construction" in script
    assert "level_save" in script
    assert "asset_delete" in script
    assert "LEVEL_WORKFLOW_SMOKE_OK" in script
    assert "-CloseLaunchedEditor" in script
