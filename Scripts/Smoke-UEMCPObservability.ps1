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
    (
        "get_output_log_filtered",
        build_output_log(limit=10, category="LogTemp", contains="get_output_log"),
    ),
    ("get_failstate_context", build_failstate_context()),
]
check_results = {name: result for name, result in checks}

failures = []
for name, result in checks:
    if not result.get("ok"):
        failures.append(f"{name} failed: {result.get('error')}")

expected_project = normalize_path(os.environ["UEMCP_SMOKE_EXPECTED_PROJECT"])
status = dict(check_results["get_editor_status"].get("data") or {})
actual_project = normalize_path(status.get("project_path", ""))
if actual_project != expected_project:
    failures.append(
        "get_editor_status returned project_path "
        f"{status.get('project_path')!r}, expected {expected_project!r}"
    )

output_log = dict(check_results["get_output_log"].get("data") or {})
output_log_entries = output_log.get("entries") or []
if not output_log_entries:
    failures.append("get_output_log returned no entries; expected live captured log entries")

placeholder_warning = "Historical output log capture is not wired yet"
for warning in output_log.get("warnings") or []:
    if placeholder_warning in str(warning):
        failures.append("get_output_log still reports placeholder historical capture warning")
        break

filtered_output_log = dict(check_results["get_output_log_filtered"].get("data") or {})
filtered_entries = filtered_output_log.get("entries") or []
if not filtered_entries:
    failures.append("get_output_log filtered query returned no entries")
for entry in filtered_entries:
    category = str(entry.get("category", ""))
    message = str(entry.get("message", ""))
    if category.lower() != "logtemp":
        failures.append(f"get_output_log filtered query returned category {category!r}")
    if "get_output_log" not in message.lower():
        failures.append("get_output_log filtered query returned an entry without the requested substring")

for entry in output_log_entries + filtered_entries:
    message = str(entry.get("message", ""))
    if "MCPServerRunnable: Sending response:" in message:
        failures.append("MCPServerRunnable is logging full response payloads into the output log")
        break

summary = {
    "ok": not failures,
    "checks": check_results,
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
