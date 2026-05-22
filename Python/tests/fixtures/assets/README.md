# Asset Workflow Smoke Fixtures

This directory contains tiny local files used by `Scripts/Smoke-UEMCPAssetWorkflow.ps1` to prove the editor-backed disk import lane without relying on marketplace, browser, account, network, or entitlement state.

The fixtures are deliberately plain source files. They are not project content until Unreal imports them through `asset_import_from_disk`.
