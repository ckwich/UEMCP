param(
    [string]$UeRoot = $env:UEMCP_UE_ROOT,
    [string]$ProjectPath,
    [string]$PluginPath,
    [string]$ProfileName = $env:UEMCP_PROFILE_NAME,
    [string]$ProfileDir = $env:UEMCP_PROFILE_DIR,
    [int]$Port = 55557,
    [int]$StartupTimeoutSeconds = 300,
    [int]$EditorReadyTimeoutSeconds = 300,
    [switch]$SkipBuild,
    [switch]$SkipLaunch,
    [switch]$CloseLaunchedEditor
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
$RepoRoot = Split-Path -Parent $ScriptDir

if (-not $UeRoot) {
    if ($IsMacOS) {
        $DefaultUeRoot = "/Users/Shared/Epic Games/UE_5.7"
    }
    elseif ($IsLinux) {
        $DefaultUeRoot = "/opt/UnrealEngine/UE_5.7"
    }
    else {
        $DefaultUeRoot = "D:\Epic\UE_5.7"
    }

    if (Test-Path -LiteralPath $DefaultUeRoot) {
        $UeRoot = $DefaultUeRoot
    }
}

if (-not $UeRoot) {
    throw "Set UEMCP_UE_ROOT or pass -UeRoot."
}

if (-not $ProjectPath) {
    $ProjectPath = Join-Path (Join-Path $RepoRoot "MCPGameProject") "MCPGameProject.uproject"
}

$DefaultProjectPath = Join-Path (Join-Path $RepoRoot "MCPGameProject") "MCPGameProject.uproject"
$DefaultPluginPath = Join-Path (Join-Path (Join-Path (Join-Path $RepoRoot "MCPGameProject") "Plugins") "UnrealMCP") "UnrealMCP.uplugin"

if ($IsMacOS) {
    $BuildPlatform = "Mac"
    $EditorPath = Join-Path $UeRoot "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
    if (-not (Test-Path -LiteralPath $EditorPath -PathType Leaf)) {
        $EditorPath = Join-Path $UeRoot "Engine/Binaries/Mac/UnrealEditor"
    }
    $BuildPath = Join-Path $UeRoot "Engine/Build/BatchFiles/RunUBT.sh"
}
elseif ($IsLinux) {
    $BuildPlatform = "Linux"
    $EditorPath = Join-Path $UeRoot "Engine/Binaries/Linux/UnrealEditor"
    $BuildPath = Join-Path $UeRoot "Engine/Build/BatchFiles/RunUBT.sh"
}
else {
    $BuildPlatform = "Win64"
    $EditorPath = Join-Path $UeRoot "Engine\Binaries\Win64\UnrealEditor.exe"
    $BuildPath = Join-Path $UeRoot "Engine\Build\BatchFiles\Build.bat"
}
$PythonDir = Join-Path $RepoRoot "Python"

foreach ($PathToCheck in @($EditorPath, $BuildPath, $ProjectPath, $PythonDir)) {
    if (-not (Test-Path -LiteralPath $PathToCheck)) {
        throw "Required path not found: $PathToCheck"
    }
}

$ResolvedProjectPath = (Resolve-Path -LiteralPath $ProjectPath).Path
$ResolvedDefaultProjectPath = (Resolve-Path -LiteralPath $DefaultProjectPath).Path
$ResolvedPluginPath = $null
$ResolvedPluginOwnerProjectPath = $null
$ResolvedProfileDir = $null

if (-not $ProfileName) {
    $ProfileName = "failstate"
}

if ($PluginPath) {
    if (-not (Test-Path -LiteralPath $PluginPath -PathType Leaf)) {
        throw "Plugin path not found: $PluginPath"
    }

    $ResolvedPluginPath = (Resolve-Path -LiteralPath $PluginPath).Path
}

if (-not $ResolvedPluginPath) {
    $ProjectPluginPath = Join-Path (Join-Path (Join-Path (Split-Path -Parent $ResolvedProjectPath) "Plugins") "UnrealMCP") "UnrealMCP.uplugin"
    if (-not (Test-Path -LiteralPath $ProjectPluginPath -PathType Leaf) -and $ResolvedProjectPath -ne $ResolvedDefaultProjectPath) {
        throw "UEMCP plugin is not attached to this project. Pass -PluginPath '$DefaultPluginPath' to launch the repo plugin through Unreal's -PLUGIN switch, or install UnrealMCP under the project's Plugins folder."
    }
}

if (-not $ProfileDir) {
    $ProjectRoot = Split-Path -Parent $ResolvedProjectPath
    $ProjectPackProfileDir = Join-Path (Join-Path (Join-Path $ProjectRoot "Tools") "UEMCP") "profiles"
    if (Test-Path -LiteralPath $ProjectPackProfileDir -PathType Container) {
        $ProfileDir = $ProjectPackProfileDir
    }
}

if ($ProfileDir) {
    if (-not (Test-Path -LiteralPath $ProfileDir -PathType Container)) {
        throw "Profile directory not found: $ProfileDir"
    }

    $ResolvedProfileDir = (Resolve-Path -LiteralPath $ProfileDir).Path
}

function Test-UemcpBridgeListener {
    $Client = [System.Net.Sockets.TcpClient]::new()
    try {
        $Connect = $Client.BeginConnect("127.0.0.1", $Port, $null, $null)
        if (-not $Connect.AsyncWaitHandle.WaitOne(1000, $false)) {
            return $false
        }

        $Client.EndConnect($Connect)
        return $true
    }
    catch {
        return $false
    }
    finally {
        $Client.Close()
    }
}

function Invoke-UemcpBridgeCommand([string]$CommandType, [hashtable]$Params = @{}, [int]$TimeoutMilliseconds = 5000) {
    $Client = [System.Net.Sockets.TcpClient]::new()
    $Memory = [System.IO.MemoryStream]::new()
    try {
        $Connect = $Client.BeginConnect("127.0.0.1", $Port, $null, $null)
        if (-not $Connect.AsyncWaitHandle.WaitOne($TimeoutMilliseconds, $false)) {
            return $null
        }

        $Client.EndConnect($Connect)
        $Client.NoDelay = $true

        $Stream = $Client.GetStream()
        $Stream.ReadTimeout = $TimeoutMilliseconds
        $Payload = @{
            type = $CommandType
            params = $Params
        } | ConvertTo-Json -Compress -Depth 20
        $PayloadBytes = [System.Text.Encoding]::UTF8.GetBytes($Payload)
        $Stream.Write($PayloadBytes, 0, $PayloadBytes.Length)

        $Buffer = New-Object byte[] 4096
        while (($BytesRead = $Stream.Read($Buffer, 0, $Buffer.Length)) -gt 0) {
            $Memory.Write($Buffer, 0, $BytesRead)
            $Text = [System.Text.Encoding]::UTF8.GetString($Memory.ToArray())
            try {
                return $Text | ConvertFrom-Json -ErrorAction Stop
            }
            catch {
                # Response JSON is not complete yet.
            }
        }

        if ($Memory.Length -eq 0) {
            return $null
        }

        $FinalText = [System.Text.Encoding]::UTF8.GetString($Memory.ToArray())
        return $FinalText | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        return [pscustomobject]@{
            status = "error"
            error = $_.Exception.Message
        }
    }
    finally {
        $Memory.Dispose()
        $Client.Close()
    }
}

function Find-PluginOwnerProjectPath([string]$PluginFilePath) {
    $SearchDir = Split-Path -Parent $PluginFilePath
    while ($SearchDir) {
        $ProjectCandidate = Get-ChildItem `
            -LiteralPath $SearchDir `
            -Filter "*.uproject" `
            -File `
            -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($ProjectCandidate) {
            return $ProjectCandidate.FullName
        }

        $ParentDir = Split-Path -Parent $SearchDir
        if (-not $ParentDir -or $ParentDir -eq $SearchDir) {
            break
        }
        $SearchDir = $ParentDir
    }

    return $null
}

function Get-UemcpEditorTargetName([string]$ProjectFilePath) {
    return "$([System.IO.Path]::GetFileNameWithoutExtension($ProjectFilePath))Editor"
}

function Invoke-UemcpEditorBuild([string]$ProjectFilePath, [string]$FailurePrefix) {
    Write-Output "Building editor target for $ProjectFilePath with $BuildPath"
    if ($BuildPlatform -eq "Win64") {
        & $BuildPath `
            Development `
            $BuildPlatform `
            "-Project=$ProjectFilePath" `
            -TargetType=Editor `
            -Progress `
            -NoEngineChanges `
            -NoHotReloadFromIDE
    }
    else {
        $TargetName = Get-UemcpEditorTargetName $ProjectFilePath
        & $BuildPath `
            $TargetName `
            $BuildPlatform `
            Development `
            "-Project=$ProjectFilePath" `
            -Progress `
            -NoEngineChanges `
            -NoHotReloadFromIDE
    }

    if ($LASTEXITCODE -ne 0) {
        throw "$FailurePrefix failed with exit code $LASTEXITCODE."
    }
}

if ($ResolvedPluginPath) {
    $PluginOwnerProjectPath = Find-PluginOwnerProjectPath $ResolvedPluginPath
    if ($PluginOwnerProjectPath) {
        $ResolvedPluginOwnerProjectPath = (Resolve-Path -LiteralPath $PluginOwnerProjectPath).Path
    }
}

if (-not $SkipBuild) {
    if ($ResolvedPluginOwnerProjectPath -and $ResolvedPluginOwnerProjectPath -ne $ResolvedProjectPath) {
        Invoke-UemcpEditorBuild $ResolvedPluginOwnerProjectPath "Plugin owner editor target build"
    }

    Invoke-UemcpEditorBuild $ResolvedProjectPath "Editor target build"
}

$LaunchedEditor = $null
$PreviousProfileDir = $env:UEMCP_PROFILE_DIR
$PreviousProfileName = $env:UEMCP_PROFILE_NAME
$PreviousExpectedProfileSourceKind = $env:UEMCP_SMOKE_EXPECTED_PROFILE_SOURCE_KIND
$PreviousRunCompatibilityGates = $env:UEMCP_SMOKE_RUN_COMPATIBILITY_GATES
try {
    if ($ResolvedProfileDir) {
        Write-Output "Using UEMCP project profile directory: $ResolvedProfileDir"
        $env:UEMCP_PROFILE_DIR = $ResolvedProfileDir
        $env:UEMCP_SMOKE_EXPECTED_PROFILE_SOURCE_KIND = "environment"
        $env:UEMCP_SMOKE_RUN_COMPATIBILITY_GATES = "1"
    }
    else {
        $env:UEMCP_SMOKE_EXPECTED_PROFILE_SOURCE_KIND = ""
        $env:UEMCP_SMOKE_RUN_COMPATIBILITY_GATES = "0"
    }
    $env:UEMCP_PROFILE_NAME = $ProfileName

    if (-not (Test-UemcpBridgeListener)) {
        if ($SkipLaunch) {
            throw "UEMCP bridge is not listening on 127.0.0.1:$Port and -SkipLaunch was passed."
        }

        Write-Output "Launching Unreal Editor for $ResolvedProjectPath"
        $EditorArgs = @($ResolvedProjectPath, "-log")
        if ($IsMacOS -or $IsLinux) {
            $EditorArgs += @("-stdout", "-FullStdOutLogOutput", "-nosplash", "-nop4", "-unattended", "-NullRHI")
        }
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
    $BridgeIsListening = $false
    while ((Get-Date) -lt $Deadline) {
        $BridgeIsListening = Test-UemcpBridgeListener
        if ($BridgeIsListening) {
            break
        }

        if ($LaunchedEditor -and $LaunchedEditor.HasExited) {
            throw "Unreal Editor exited before UEMCP bridge opened on 127.0.0.1:$Port."
        }

        Start-Sleep -Seconds 2
    }

    if (-not $BridgeIsListening) {
        throw "Timed out waiting for UEMCP bridge on 127.0.0.1:$Port."
    }

    Write-Output "UEMCP bridge listening on 127.0.0.1:$Port."

    $EditorReadyDeadline = (Get-Date).AddSeconds($EditorReadyTimeoutSeconds)
    $EditorReadyResponse = $null
    while ((Get-Date) -lt $EditorReadyDeadline) {
        $EditorReadyResponse = Invoke-UemcpBridgeCommand `
            -CommandType "get_editor_status" `
            -Params @{} `
            -TimeoutMilliseconds 5000
        if ($EditorReadyResponse -and $EditorReadyResponse.status -eq "success") {
            break
        }

        if ($LaunchedEditor -and $LaunchedEditor.HasExited) {
            throw "Unreal Editor exited before UEMCP editor-backed commands became ready."
        }

        Start-Sleep -Seconds 2
    }

    if (-not $EditorReadyResponse -or $EditorReadyResponse.status -ne "success") {
        $LastEditorReadyError = if ($EditorReadyResponse) { $EditorReadyResponse.error } else { "no response" }
        throw "Timed out waiting for UEMCP editor-backed commands on 127.0.0.1:$Port. Last status: $LastEditorReadyError"
    }

    Write-Output "UEMCP editor-backed commands ready on 127.0.0.1:$Port."

    $env:UEMCP_SMOKE_EXPECTED_PROJECT = $ResolvedProjectPath

    $ProbeScript = @'
import json
import os
import sys
from pathlib import Path

from tools.observability_tools import (
    build_asset_search,
    build_automation_test_run,
    build_automation_tests,
    build_editor_automation_readiness_diagnostic,
    build_editor_readiness,
    build_editor_status,
    build_level_snapshot,
    build_observability_recent_events,
    build_observability_state_summary,
    build_output_log,
    build_profile_automation_run,
    build_project_compatibility_gates,
    build_project_context,
    build_uemcp_ping,
)


def normalize_path(value: str) -> str:
    return Path(value).resolve().as_posix().lower()


expected_project = normalize_path(os.environ["UEMCP_SMOKE_EXPECTED_PROJECT"])
profile_name = os.environ.get("UEMCP_PROFILE_NAME", "failstate")
expected_profile_source_kind = os.environ.get("UEMCP_SMOKE_EXPECTED_PROFILE_SOURCE_KIND", "")
run_compatibility_gates = os.environ.get("UEMCP_SMOKE_RUN_COMPATIBILITY_GATES") == "1"


checks = []

checks.append(("uemcp_ping", build_uemcp_ping()))
checks.append(("get_editor_status", build_editor_status()))
checks.append(("get_output_log", build_output_log(limit=5)))
checks.append(("get_level_snapshot", build_level_snapshot(limit=25)))
checks.append(("asset_search_game_root", build_asset_search(root="/Game", limit=20)))
checks.append(
    (
        "get_output_log_filtered",
        build_output_log(limit=10, category="LogTemp", contains="get_output_log"),
    )
)
checks.append(("get_project_context", build_project_context(profile_name=profile_name)))
checks.append(
    (
        "diagnose_editor_automation_readiness",
        build_editor_automation_readiness_diagnostic(
            readiness_timeout_seconds=60,
            readiness_stable_samples=2,
            output_log_limit=5,
        ),
    )
)
checks.append(
    (
        "get_editor_readiness_before_discovery",
        build_editor_readiness(timeout_seconds=90, stable_samples=2, settle_seconds=20),
    )
)
checks.append(("list_uemcp_automation_tests", build_automation_tests(prefix="UEMCP.", limit=20)))
checks.append(
    (
        "get_editor_readiness_before_uemcp_run",
        build_editor_readiness(timeout_seconds=60, stable_samples=2),
    )
)
checks.append(
    (
        "run_uemcp_automation_smoke",
        build_automation_test_run(
            test_name="UEMCP.Observability.Smoke",
            timeout_seconds=30,
        ),
    )
)
checks.append(
    (
        "run_profile_automation_uemcp_smoke",
        build_profile_automation_run(
            profile_name=profile_name,
            test_name="UEMCP.Observability.Smoke",
            timeout_seconds=30,
            output_log_limit=5,
            require_ready=True,
            readiness_timeout_seconds=60,
            readiness_stable_samples=2,
        ),
    )
)

if run_compatibility_gates:
    checks.append(
        (
            "run_project_compatibility_gates",
            build_project_compatibility_gates(
                profile_name=profile_name,
                limit=10,
                timeout_seconds=30,
                output_log_limit=10,
                require_ready=True,
                readiness_timeout_seconds=60,
                readiness_stable_samples=2,
            ),
        )
    )

checks.append(("get_observability_recent_events", build_observability_recent_events(limit=50)))
checks.append(("summarize_observability_state", build_observability_state_summary(limit=50)))

check_results = {name: result for name, result in checks}

failures = []
for name, result in checks:
    if not result.get("ok"):
        failures.append(f"{name} failed: {result.get('error')}")
    if name.startswith("get_editor_readiness"):
        readiness = dict(result.get("data") or {})
        if readiness.get("ready") is not True:
            failures.append(f"{name} did not report ready editor state: {readiness}")

status = dict(check_results["get_editor_status"].get("data") or {})
actual_project = normalize_path(status.get("project_path", ""))
if actual_project != expected_project:
    failures.append(
        "get_editor_status returned project_path "
        f"{status.get('project_path')!r}, expected {expected_project!r}"
    )

project_context = dict(check_results["get_project_context"].get("data") or {})
profile_source = dict(project_context.get("profile_source") or {})
if expected_profile_source_kind and profile_source.get("kind") != expected_profile_source_kind:
    failures.append(
        "Smoke did not load the expected UEMCP profile source kind "
        f"{expected_profile_source_kind!r}: {profile_source}"
    )

output_log = dict(check_results["get_output_log"].get("data") or {})
output_log_entries = output_log.get("entries") or []
if not output_log_entries:
    failures.append("get_output_log returned no entries; expected live captured log entries")

level_snapshot = dict(check_results["get_level_snapshot"].get("data") or {})
level_snapshot_filters = dict(level_snapshot.get("filters") or {})
if level_snapshot_filters.get("limit") != 25:
    failures.append(f"get_level_snapshot did not report the requested limit: {level_snapshot}")
if level_snapshot.get("returned_actor_count", 0) > 25:
    failures.append(f"get_level_snapshot returned more actors than its limit: {level_snapshot}")
if level_snapshot.get("total_actor_count", 0) < level_snapshot.get("returned_actor_count", 0):
    failures.append(f"get_level_snapshot returned impossible actor counts: {level_snapshot}")
for actor in level_snapshot.get("actors") or []:
    for field_name in ("name", "class", "path", "location", "rotation", "scale"):
        if not actor.get(field_name):
            failures.append(f"get_level_snapshot actor missing {field_name}: {actor}")

asset_search = dict(check_results["asset_search_game_root"].get("data") or {})
asset_search_filters = dict(asset_search.get("filters") or {})
if asset_search_filters.get("package_path") != "/Game":
    failures.append(f"asset_search did not normalize /Game root: {asset_search}")
if asset_search.get("returned_asset_count", 0) > 20:
    failures.append(f"asset_search returned more assets than its limit: {asset_search}")
for asset in asset_search.get("assets") or []:
    package_path = str(asset.get("package_path") or "")
    if not package_path.startswith("/Game"):
        failures.append(f"asset_search returned an asset outside /Game: {asset}")
    for field_name in ("asset_name", "object_path", "package_name", "package_path", "asset_class"):
        if not asset.get(field_name):
            failures.append(f"asset_search asset missing {field_name}: {asset}")

diagnostic = dict(check_results["diagnose_editor_automation_readiness"].get("data") or {})
diagnostic_summary = dict(diagnostic.get("summary") or {})
diagnostic_gates = dict(diagnostic.get("gates") or {})
if diagnostic.get("ready_for_automation") is not True:
    failures.append(f"diagnose_editor_automation_readiness did not report ready: {diagnostic}")
if diagnostic_summary.get("state") != "ready":
    failures.append(
        "diagnose_editor_automation_readiness returned unexpected summary state: "
        f"{diagnostic_summary}"
    )
for gate_name in ("bridge", "status", "readiness", "output_log"):
    gate = dict(diagnostic_gates.get(gate_name) or {})
    if gate.get("ok") is not True:
        failures.append(f"diagnose_editor_automation_readiness gate {gate_name!r} failed: {gate}")
if (diagnostic_gates.get("readiness") or {}).get("ready") is not True:
    failures.append(
        "diagnose_editor_automation_readiness readiness gate did not report ready: "
        f"{diagnostic_gates.get('readiness')}"
    )
if diagnostic.get("observability_events") != []:
    failures.append(
        "diagnose_editor_automation_readiness returned unexpected observability events: "
        f"{diagnostic.get('observability_events')}"
    )
diagnostic_refs = dict(diagnostic.get("evidence_refs") or {})
for ref_name in (
    "ping_request_id",
    "status_request_id",
    "readiness_request_id",
    "output_log_request_id",
):
    if not diagnostic_refs.get(ref_name):
        failures.append(f"diagnose_editor_automation_readiness missing evidence ref {ref_name}")

recent_events = dict(check_results["get_observability_recent_events"].get("data") or {})
recent_entries = recent_events.get("entries") or []
recent_tools = {str(entry.get("tool")) for entry in recent_entries}
required_recent_tools = [
    "get_editor_readiness",
    "diagnose_editor_automation_readiness",
    "run_profile_automation_tests",
]
if run_compatibility_gates:
    required_recent_tools.append("run_project_compatibility_gates")
for required_tool in required_recent_tools:
    if required_tool not in recent_tools:
        failures.append(
            "get_observability_recent_events did not return recent "
            f"{required_tool!r} history: {recent_events}"
        )
if "get_observability_recent_events" in recent_tools:
    failures.append("get_observability_recent_events recorded its own read into history")
for entry in recent_entries:
    if entry.get("successful") is not True:
        failures.append(f"get_observability_recent_events returned an unexpected failure: {entry}")
if recent_events.get("history_capacity") != 100:
    failures.append(
        "get_observability_recent_events returned unexpected history capacity: "
        f"{recent_events}"
    )

state_summary = dict(check_results["summarize_observability_state"].get("data") or {})
latest_state_entry = dict(state_summary.get("latest_entry") or {})
if state_summary.get("state") != "ready":
    failures.append(f"summarize_observability_state did not report ready: {state_summary}")
if state_summary.get("latest_blocker") is not None:
    failures.append(
        "summarize_observability_state reported an unexpected blocker during passing smoke: "
        f"{state_summary.get('latest_blocker')}"
    )
if state_summary.get("recommended_next_step") is not None:
    failures.append(
        "summarize_observability_state returned an unexpected next step during passing smoke: "
        f"{state_summary.get('recommended_next_step')}"
    )
if latest_state_entry.get("successful") is not True:
    failures.append(
        "summarize_observability_state latest entry was not successful: "
        f"{latest_state_entry}"
    )
if latest_state_entry.get("tool") not in recent_tools:
    failures.append(
        "summarize_observability_state latest entry was not present in recent history: "
        f"{latest_state_entry}"
    )
summary_counts = dict(state_summary.get("counts") or {})
if summary_counts.get("unsuccessful") != 0:
    failures.append(f"summarize_observability_state reported failures: {state_summary}")
if state_summary.get("history_capacity") != 100:
    failures.append(
        "summarize_observability_state returned unexpected history capacity: "
        f"{state_summary}"
    )

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

uemcp_tests = dict(check_results["list_uemcp_automation_tests"].get("data") or {})
uemcp_test_paths = {
    str(test.get("full_test_path") or test.get("test_name") or "")
    for test in uemcp_tests.get("tests") or []
}
if "UEMCP.Observability.Smoke" not in uemcp_test_paths:
    failures.append("list_automation_tests did not return UEMCP.Observability.Smoke")

smoke_run = dict(check_results["run_uemcp_automation_smoke"].get("data") or {})
if smoke_run.get("status") != "passed" or smoke_run.get("successful") is not True:
    failures.append(f"run_automation_test did not pass UEMCP.Observability.Smoke: {smoke_run}")

profile_smoke_run = dict(check_results["run_profile_automation_uemcp_smoke"].get("data") or {})
profile_smoke_summary = dict(profile_smoke_run.get("summary") or {})
profile_smoke_readiness = dict(profile_smoke_run.get("readiness") or {})
if profile_smoke_run.get("mode") != "single":
    failures.append(f"run_profile_automation_tests did not use single mode: {profile_smoke_run}")
if profile_smoke_readiness.get("ready") is not True:
    failures.append(
        "run_profile_automation_tests did not gate UEMCP smoke on editor readiness: "
        f"{profile_smoke_run}"
    )
if not (profile_smoke_run.get("evidence_refs") or {}).get("readiness_request_id"):
    failures.append("run_profile_automation_tests did not return UEMCP readiness evidence")
if profile_smoke_run.get("observability_events") != []:
    failures.append(
        "run_profile_automation_tests returned unexpected UEMCP observability events: "
        f"{profile_smoke_run.get('observability_events')}"
    )
if profile_smoke_summary.get("total") != 1 or profile_smoke_summary.get("successful") is not True:
    failures.append(
        "run_profile_automation_tests did not pass UEMCP.Observability.Smoke: "
        f"{profile_smoke_run}"
    )
if not (profile_smoke_run.get("output_log_tail") or {}).get("entries"):
    failures.append("run_profile_automation_tests did not return output_log_tail entries")

if run_compatibility_gates:
    compatibility_run = dict(check_results["run_project_compatibility_gates"].get("data") or {})
    compatibility_summary = dict(compatibility_run.get("summary") or {})
    compatibility_readiness = dict(compatibility_run.get("readiness") or {})
    compatibility_profile_source = dict(compatibility_run.get("profile_source") or {})
    compatibility_gates = compatibility_run.get("gates") or []
    if compatibility_summary.get("total", 0) < 1:
        failures.append(
            "run_project_compatibility_gates returned no project compatibility gates: "
            f"{compatibility_run}"
        )
    if compatibility_summary.get("successful") is not True:
        failures.append(
            "run_project_compatibility_gates did not pass all project compatibility gates: "
            f"{compatibility_run}"
        )
    if compatibility_readiness.get("ready") is not True:
        failures.append(
            "run_project_compatibility_gates did not gate execution on editor readiness: "
            f"{compatibility_run}"
        )
    if expected_profile_source_kind and compatibility_profile_source.get("kind") != expected_profile_source_kind:
        failures.append(
            "run_project_compatibility_gates used unexpected profile source: "
            f"{compatibility_profile_source}"
        )
    if compatibility_run.get("observability_events") != []:
        failures.append(
            "run_project_compatibility_gates returned unexpected observability events: "
            f"{compatibility_run.get('observability_events')}"
        )
    for gate in compatibility_gates:
        if gate.get("ok") is not True:
            failures.append(f"Project compatibility gate failed: {gate}")

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
    $env:UEMCP_PROFILE_DIR = $PreviousProfileDir
    $env:UEMCP_PROFILE_NAME = $PreviousProfileName
    $env:UEMCP_SMOKE_EXPECTED_PROFILE_SOURCE_KIND = $PreviousExpectedProfileSourceKind
    $env:UEMCP_SMOKE_RUN_COMPATIBILITY_GATES = $PreviousRunCompatibilityGates

    if ($CloseLaunchedEditor -and $LaunchedEditor -and -not $LaunchedEditor.HasExited) {
        Write-Output "Closing launched Unreal Editor PID $($LaunchedEditor.Id)."
        Stop-Process -Id $LaunchedEditor.Id
    }
}
