from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def test_interactive_asset_workflow_script_is_separate_from_headless_smoke():
    script_path = REPO_ROOT / "Scripts" / "Start-UEMCPInteractiveAssetWorkflow.ps1"
    script = script_path.read_text(encoding="utf-8")

    assert "Interactive asset workflows require an explicit consuming project -ProjectPath" in script
    assert "AllowHarnessProject" in script
    assert "ASSET_WORKFLOW_PREFLIGHT_OK" in script
    assert "ASSET_WORKFLOW_STARTED" in script
    assert "-PLUGIN=$ResolvedPluginPath" in script
    assert "-NullRHI" not in script
    assert "-unattended" not in script
    assert "-stdout" not in script
    assert "-FullStdOutLogOutput" not in script


def test_interactive_asset_workflow_preflights_fab_without_reenabling_smoke_fab():
    script = (REPO_ROOT / "Scripts" / "Start-UEMCPInteractiveAssetWorkflow.ps1").read_text(
        encoding="utf-8"
    )
    project = (REPO_ROOT / "MCPGameProject" / "MCPGameProject.uproject").read_text(
        encoding="utf-8"
    )

    assert "Engine/Plugins/Fab/Fab.uplugin" in script
    assert "Fab is explicitly disabled in this project" in script
    assert "NoFabRequirement" in script
    assert "FAB_CHECK_OK" in script
    assert '"Name": "Fab"' in project
    assert '"Enabled": false' in project


def test_interactive_asset_workflow_docs_define_agent_ladder_and_stop_rules():
    docs = (REPO_ROOT / "Docs" / "Workflows" / "interactive-asset-workflow.md").read_text(
        encoding="utf-8"
    )
    docs_index = (REPO_ROOT / "Docs" / "README.md").read_text(encoding="utf-8")
    plan = (
        REPO_ROOT
        / "Docs"
        / "superpowers"
        / "plans"
        / "2026-05-22-uemcp-interactive-asset-workflows.md"
    ).read_text(encoding="utf-8")

    assert "Interactive asset work is a separate UEMCP lane" in docs
    assert "Fab account actions remain interactive" in docs
    assert "Asset Registry state before and after an import" in docs
    assert "Stop and ask the user" in docs
    assert "Interactive Asset Workflow" in docs_index
    assert "asset_intake_snapshot" in plan
    assert "asset_import_from_disk" in plan
