from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def test_interactive_asset_workflow_script_is_separate_from_headless_smoke():
    script_path = REPO_ROOT / "Scripts" / "Start-UEMCPInteractiveAssetWorkflow.ps1"
    script = script_path.read_text(encoding="utf-8")

    assert "Interactive asset workflows require an explicit consuming project -ProjectPath" in script
    assert "AllowHarnessProject" in script
    assert "ExpectedAssetRoots" in script
    assert "ASSET_WORKFLOW_EXPECTED_ROOTS" in script
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


def test_fab_assisted_import_protocol_keeps_marketplace_actions_interactive():
    script = (REPO_ROOT / "Scripts" / "Start-UEMCPInteractiveAssetWorkflow.ps1").read_text(
        encoding="utf-8"
    )
    docs = (REPO_ROOT / "Docs" / "Workflows" / "fab-assisted-import.md").read_text(
        encoding="utf-8"
    )

    assert "purchase" not in script.lower()
    assert "license" not in script.lower()
    assert "login" not in script.lower()
    assert "does not automate Fab login, entitlement, purchase, license acceptance, or payment" in docs
    assert "asset_intake_write_manifest" in docs
    assert "source URL" in docs


def test_asset_workflow_smoke_script_uses_fixture_import_without_fab_dependency():
    script = (REPO_ROOT / "Scripts" / "Smoke-UEMCPAssetWorkflow.ps1").read_text(
        encoding="utf-8"
    )
    fixture_readme = (
        REPO_ROOT / "Python" / "tests" / "fixtures" / "assets" / "README.md"
    ).read_text(encoding="utf-8")

    assert "SkipFab" in script
    assert "asset_intake_snapshot" in script
    assert "asset_import_from_disk" in script
    assert "asset_intake_diff" in script
    assert "asset_validate_level_placements" in script
    assert "Fab" not in fixture_readme
