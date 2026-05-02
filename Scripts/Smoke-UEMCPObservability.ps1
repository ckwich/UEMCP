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
    build_asset_search,
    build_automation_test_run,
    build_automation_tests,
    build_editor_automation_readiness_diagnostic,
    build_editor_readiness,
    build_editor_status,
    build_failstate_context,
    build_observability_recent_events,
    build_observability_state_summary,
    build_output_log,
    build_profile_automation_run,
    build_uemcp_ping,
)


def normalize_path(value: str) -> str:
    return Path(value).resolve().as_posix().lower()


expected_project = normalize_path(os.environ["UEMCP_SMOKE_EXPECTED_PROJECT"])
is_failstate_project = "/failstate/" in expected_project or expected_project.endswith("/failstate.uproject")


checks = []

checks.append(("uemcp_ping", build_uemcp_ping()))
checks.append(("get_editor_status", build_editor_status()))
checks.append(("get_output_log", build_output_log(limit=5)))
checks.append(("asset_search_game_root", build_asset_search(root="/Game", limit=20)))
checks.append(
    (
        "get_output_log_filtered",
        build_output_log(limit=10, category="LogTemp", contains="get_output_log"),
    )
)
checks.append(("get_failstate_context", build_failstate_context()))
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
            test_name="UEMCP.Observability.Smoke",
            timeout_seconds=30,
            output_log_limit=5,
            require_ready=True,
            readiness_timeout_seconds=60,
            readiness_stable_samples=2,
        ),
    )
)

if is_failstate_project:
    checks.append(
        (
            "asset_search_failstate_blockout",
            build_asset_search(
                root="Content/Failstate/Blueprints/Blockout",
                name_contains="BP_FSBlockout",
                limit=50,
            ),
        )
    )
    checks.append(
        (
            "get_editor_readiness_before_failstate_discovery",
            build_editor_readiness(timeout_seconds=60, stable_samples=2),
        )
    )
    checks.append(
        (
            "list_failstate_automation_tests",
            build_automation_tests(prefix="Failstate.Phase1", limit=20),
        )
    )
    checks.append(
        (
            "run_profile_automation_failstate_prefix",
            build_profile_automation_run(
                profile_name="failstate",
                prefix="Failstate.Phase1",
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

output_log = dict(check_results["get_output_log"].get("data") or {})
output_log_entries = output_log.get("entries") or []
if not output_log_entries:
    failures.append("get_output_log returned no entries; expected live captured log entries")

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
for required_tool in (
    "get_editor_readiness",
    "diagnose_editor_automation_readiness",
    "run_profile_automation_tests",
):
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

if is_failstate_project:
    failstate_asset_search = dict(check_results["asset_search_failstate_blockout"].get("data") or {})
    failstate_asset_filters = dict(failstate_asset_search.get("filters") or {})
    failstate_assets = failstate_asset_search.get("assets") or []
    failstate_asset_names = {
        str(asset.get("asset_name") or "")
        for asset in failstate_assets
    }
    expected_blockout_assets = {
        "BP_FSBlockoutCover",
        "BP_FSBlockoutFloor",
        "BP_FSBlockoutVisualOnly",
    }
    if failstate_asset_filters.get("package_path") != "/Game/Failstate/Blueprints/Blockout":
        failures.append(
            "asset_search did not normalize Failstate blockout root: "
            f"{failstate_asset_search}"
        )
    if failstate_asset_search.get("matched_asset_count", 0) < len(expected_blockout_assets):
        failures.append(
            "asset_search did not find enough Failstate blockout assets: "
            f"{failstate_asset_search}"
        )
    missing_blockout_assets = expected_blockout_assets - failstate_asset_names
    if missing_blockout_assets:
        failures.append(
            "asset_search missed expected Failstate blockout assets: "
            f"{sorted(missing_blockout_assets)}"
        )
    for asset in failstate_assets:
        package_path = str(asset.get("package_path") or "")
        if not package_path.startswith("/Game/Failstate/Blueprints/Blockout"):
            failures.append(f"asset_search returned outside Failstate blockout root: {asset}")

    failstate_tests = dict(check_results["list_failstate_automation_tests"].get("data") or {})
    returned_failstate_tests = failstate_tests.get("tests") or []
    if not returned_failstate_tests:
        failures.append("list_automation_tests returned no Failstate.Phase1 tests")
    for test in returned_failstate_tests:
        full_test_path = str(test.get("full_test_path") or test.get("test_name") or "")
        if not full_test_path.startswith("Failstate.Phase1"):
            failures.append(f"Failstate automation query returned outside-prefix test {full_test_path!r}")

    failstate_profile_run = dict(
        check_results["run_profile_automation_failstate_prefix"].get("data") or {}
    )
    failstate_profile_summary = dict(failstate_profile_run.get("summary") or {})
    failstate_profile_readiness = dict(failstate_profile_run.get("readiness") or {})
    if failstate_profile_run.get("mode") != "prefix":
        failures.append(
            f"run_profile_automation_tests did not use Failstate prefix mode: {failstate_profile_run}"
        )
    if failstate_profile_readiness.get("ready") is not True:
        failures.append(
            "run_profile_automation_tests did not gate Failstate prefix on editor readiness: "
            f"{failstate_profile_run}"
        )
    if not (failstate_profile_run.get("evidence_refs") or {}).get("readiness_request_id"):
        failures.append("run_profile_automation_tests did not return Failstate readiness evidence")
    if failstate_profile_run.get("observability_events") != []:
        failures.append(
            "run_profile_automation_tests returned unexpected Failstate observability events: "
            f"{failstate_profile_run.get('observability_events')}"
        )
    if failstate_profile_run.get("prefix") != "Failstate.Phase1":
        failures.append(
            "run_profile_automation_tests used unexpected Failstate prefix: "
            f"{failstate_profile_run.get('prefix')!r}"
        )
    if failstate_profile_summary.get("total", 0) < 1:
        failures.append("run_profile_automation_tests returned no Failstate.Phase1 results")
    if failstate_profile_summary.get("successful") is not True:
        failures.append(
            "run_profile_automation_tests did not pass the Failstate.Phase1 prefix batch: "
            f"{failstate_profile_run}"
        )

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
