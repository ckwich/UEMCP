param(
    [string]$UeRoot = $env:UEMCP_UE_ROOT,
    [string]$ProjectPath,
    [string]$PluginPath,
    [int]$Port = 55557,
    [int]$StartupTimeoutSeconds = 300,
    [switch]$SkipBuild,
    [switch]$SkipLaunch,
    [switch]$CloseLaunchedEditor
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
$RepoRoot = Split-Path -Parent $ScriptDir

if (-not $UeRoot) {
    $DefaultUeRoot = "D:\Epic\UE_5.7"
    if (Test-Path -LiteralPath $DefaultUeRoot) {
        $UeRoot = $DefaultUeRoot
    }
}

if (-not $UeRoot) {
    throw "Set UEMCP_UE_ROOT or pass -UeRoot."
}

if (-not $ProjectPath) {
    $ProjectPath = Join-Path $RepoRoot "MCPGameProject\MCPGameProject.uproject"
}

$DefaultProjectPath = Join-Path $RepoRoot "MCPGameProject\MCPGameProject.uproject"
$DefaultPluginPath = Join-Path $RepoRoot "MCPGameProject\Plugins\UnrealMCP\UnrealMCP.uplugin"

$EditorPath = Join-Path $UeRoot "Engine\Binaries\Win64\UnrealEditor.exe"
$BuildPath = Join-Path $UeRoot "Engine\Build\BatchFiles\Build.bat"
$PythonDir = Join-Path $RepoRoot "Python"

foreach ($PathToCheck in @($EditorPath, $BuildPath, $ProjectPath, $PythonDir)) {
    if (-not (Test-Path -LiteralPath $PathToCheck)) {
        throw "Required path not found: $PathToCheck"
    }
}

$ResolvedProjectPath = (Resolve-Path -LiteralPath $ProjectPath).Path
$ResolvedDefaultProjectPath = (Resolve-Path -LiteralPath $DefaultProjectPath).Path
$ResolvedPluginPath = $null

if ($PluginPath) {
    if (-not (Test-Path -LiteralPath $PluginPath -PathType Leaf)) {
        throw "Plugin path not found: $PluginPath"
    }

    $ResolvedPluginPath = (Resolve-Path -LiteralPath $PluginPath).Path
}

if (-not $ResolvedPluginPath) {
    $ProjectPluginPath = Join-Path (Split-Path -Parent $ResolvedProjectPath) "Plugins\UnrealMCP\UnrealMCP.uplugin"
    if (-not (Test-Path -LiteralPath $ProjectPluginPath -PathType Leaf) -and $ResolvedProjectPath -ne $ResolvedDefaultProjectPath) {
        throw "UEMCP plugin is not attached to this project. Pass -PluginPath '$DefaultPluginPath' to launch the repo plugin through Unreal's -PLUGIN switch, or install UnrealMCP under the project's Plugins folder."
    }
}

function Get-UemcpBridgeListener {
    Get-NetTCPConnection `
        -LocalAddress 127.0.0.1 `
        -LocalPort $Port `
        -State Listen `
        -ErrorAction SilentlyContinue |
        Select-Object -First 1
}

if (-not $SkipBuild) {
    Write-Output "Building editor target for $ResolvedProjectPath with $BuildPath"
    & $BuildPath `
        Development `
        Win64 `
        "-Project=$ResolvedProjectPath" `
        -TargetType=Editor `
        -Progress `
        -NoEngineChanges `
        -NoHotReloadFromIDE

    if ($LASTEXITCODE -ne 0) {
        throw "Editor target build failed with exit code $LASTEXITCODE."
    }
}

$LaunchedEditor = $null
try {
    if (-not (Get-UemcpBridgeListener)) {
        if ($SkipLaunch) {
            throw "UEMCP bridge is not listening on 127.0.0.1:$Port and -SkipLaunch was passed."
        }

        Write-Output "Launching Unreal Editor for $ResolvedProjectPath"
        $EditorArgs = @($ResolvedProjectPath, "-log")
        if ($ResolvedPluginPath) {
            Write-Output "Attaching UEMCP plugin with -PLUGIN=$ResolvedPluginPath"
            $EditorArgs += "-PLUGIN=$ResolvedPluginPath"
        }

        $LaunchedEditor = Start-Process `
            -FilePath $EditorPath `
            -ArgumentList $EditorArgs `
            -PassThru
    }

    $Deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
    $Listener = $null
    while ((Get-Date) -lt $Deadline) {
        $Listener = Get-UemcpBridgeListener
        if ($Listener) {
            break
        }

        if ($LaunchedEditor -and $LaunchedEditor.HasExited) {
            throw "Unreal Editor exited before UEMCP bridge opened on 127.0.0.1:$Port."
        }

        Start-Sleep -Seconds 2
    }

    if (-not $Listener) {
        throw "Timed out waiting for UEMCP bridge on 127.0.0.1:$Port."
    }

    Write-Output "UEMCP bridge listening on 127.0.0.1:$Port (PID $($Listener.OwningProcess))."

    $env:UEMCP_SMOKE_EXPECTED_PROJECT = $ResolvedProjectPath

    $ProbeScript = @'
import json
import os
import sys
from pathlib import Path

from tools.observability_tools import (
    build_editor_status,
    build_failstate_context,
    build_output_log,
    build_uemcp_ping,
)


def normalize_path(value: str) -> str:
    return Path(value).resolve().as_posix().lower()


checks = [
    ("uemcp_ping", build_uemcp_ping()),
    ("get_editor_status", build_editor_status()),
    ("get_output_log", build_output_log(limit=5)),
    ("get_failstate_context", build_failstate_context()),
]

failures = []
for name, result in checks:
    if not result.get("ok"):
        failures.append(f"{name} failed: {result.get('error')}")

expected_project = normalize_path(os.environ["UEMCP_SMOKE_EXPECTED_PROJECT"])
status = dict(checks[1][1].get("data") or {})
actual_project = normalize_path(status.get("project_path", ""))
if actual_project != expected_project:
    failures.append(
        "get_editor_status returned project_path "
        f"{status.get('project_path')!r}, expected {expected_project!r}"
    )

summary = {
    "ok": not failures,
    "checks": {name: result for name, result in checks},
    "expected_project": expected_project,
    "failures": failures,
}
print(json.dumps(summary, indent=2, sort_keys=True))

if failures:
    sys.exit(1)

print("SMOKE_OK")
'@

    Push-Location -LiteralPath $PythonDir
    try {
        $ProbeScript | uv run python -
    }
    finally {
        Pop-Location
    }

    if ($LASTEXITCODE -ne 0) {
        throw "UEMCP observability smoke failed with exit code $LASTEXITCODE."
    }
}
finally {
    if ($CloseLaunchedEditor -and $LaunchedEditor -and -not $LaunchedEditor.HasExited) {
        Write-Output "Closing launched Unreal Editor PID $($LaunchedEditor.Id)."
        Stop-Process -Id $LaunchedEditor.Id
    }
}
