import json
import tomllib
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def test_python_runtime_excludes_unsupported_python_314():
    pyproject = tomllib.loads((REPO_ROOT / "Python" / "pyproject.toml").read_text())

    assert pyproject["project"]["requires-python"] == ">=3.10,<3.14"


def test_pytest_console_entrypoint_can_import_local_modules():
    pyproject = tomllib.loads((REPO_ROOT / "Python" / "pyproject.toml").read_text())

    assert pyproject["tool"]["pytest"]["ini_options"]["pythonpath"] == ["."]


def test_mcp_config_uses_portable_repo_python_directory():
    config = json.loads((REPO_ROOT / "mcp.json").read_text())

    args = config["mcpServers"]["uemcp"]["args"]
    assert args[:2] == ["--directory", "Python"]
    assert not any("C:/Dev" in arg or "C:\\Dev" in arg for arg in args)


def test_smoke_script_has_macos_unreal_paths_and_socket_probe():
    script = (REPO_ROOT / "Scripts" / "Smoke-UEMCPObservability.ps1").read_text()

    assert "$IsMacOS" in script
    assert "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" in script
    assert "Engine/Build/BatchFiles/RunUBT.sh" in script
    assert "[System.Net.Sockets.TcpClient]" in script
    assert "EditorReadyTimeoutSeconds" in script
    assert "UEMCP editor-backed commands ready" in script
    assert "Get-NetTCPConnection" not in script


def test_smoke_project_disables_unneeded_fab_editor_plugin():
    project = json.loads((REPO_ROOT / "MCPGameProject" / "MCPGameProject.uproject").read_text())

    plugin_settings = {plugin["Name"]: plugin for plugin in project["Plugins"]}
    assert plugin_settings["Fab"]["Enabled"] is False


def test_smoke_project_uses_lightweight_default_map():
    default_engine = (REPO_ROOT / "MCPGameProject" / "Config" / "DefaultEngine.ini").read_text()

    assert "EditorStartupMap=/Engine/Maps/Templates/Template_Default" in default_engine
    assert "GameDefaultMap=/Engine/Maps/Templates/Template_Default" in default_engine
    assert "EditorStartupMap=/Engine/Maps/Templates/OpenWorld" not in default_engine
    assert "GameDefaultMap=/Engine/Maps/Templates/OpenWorld" not in default_engine
