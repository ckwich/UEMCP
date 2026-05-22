param(
    [string]$UeRoot = $env:UEMCP_UE_ROOT,
    [string]$ProjectPath,
    [string]$PluginPath,
    [string]$ProfileName = $env:UEMCP_PROFILE_NAME,
    [string]$ProfileDir = $env:UEMCP_PROFILE_DIR,
    [string]$ImportRoot = "/Game/UEMCP/Smoke",
    [string]$FixtureAssetPath,
    [string[]]$ExpectedActors = @(),
    [int]$Port = 55557,
    [int]$StartupTimeoutSeconds = 300,
    [int]$CommandTimeoutMilliseconds = 30000,
    [switch]$SkipBuild,
    [switch]$SkipLaunch,
    [switch]$SkipFab,
    [switch]$KeepImportedAssets,
    [switch]$CloseLaunchedEditor
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
$RepoRoot = Split-Path -Parent $ScriptDir
$PythonDir = Join-Path $RepoRoot "Python"
$StartScript = Join-Path $ScriptDir "Start-UEMCPInteractiveAssetWorkflow.ps1"

if (-not $FixtureAssetPath) {
    $FixtureAssetPath = Join-Path (Join-Path (Join-Path $PythonDir "tests") "fixtures") "assets/SM_UEMCP_Smoke.obj"
}

foreach ($PathToCheck in @($StartScript, $PythonDir, $FixtureAssetPath)) {
    if (-not (Test-Path -LiteralPath $PathToCheck)) {
        throw "Required asset workflow smoke path not found: $PathToCheck"
    }
}

function Invoke-UemcpBridgeCommand([string]$CommandType, [hashtable]$Params = @{}, [int]$TimeoutMilliseconds = 30000) {
    $Client = [System.Net.Sockets.TcpClient]::new()
    $Memory = [System.IO.MemoryStream]::new()
    try {
        $Connect = $Client.BeginConnect("127.0.0.1", $Port, $null, $null)
        if (-not $Connect.AsyncWaitHandle.WaitOne($TimeoutMilliseconds, $false)) {
            throw "Timed out connecting to UEMCP bridge on 127.0.0.1:$Port"
        }

        $Client.EndConnect($Connect)
        $Client.NoDelay = $true

        $Stream = $Client.GetStream()
        $Stream.ReadTimeout = $TimeoutMilliseconds
        $Payload = @{
            type = $CommandType
            params = $Params
        } | ConvertTo-Json -Compress -Depth 50
        $PayloadBytes = [System.Text.Encoding]::UTF8.GetBytes($Payload)
        $Stream.Write($PayloadBytes, 0, $PayloadBytes.Length)

        $Buffer = New-Object byte[] 8192
        while (($BytesRead = $Stream.Read($Buffer, 0, $Buffer.Length)) -gt 0) {
            $Memory.Write($Buffer, 0, $BytesRead)
            $Text = [System.Text.Encoding]::UTF8.GetString($Memory.ToArray())
            try {
                return $Text | ConvertFrom-Json -ErrorAction Stop
            }
            catch {
                # Response JSON is still streaming.
            }
        }

        if ($Memory.Length -eq 0) {
            throw "UEMCP bridge returned no response for $CommandType"
        }

        $FinalText = [System.Text.Encoding]::UTF8.GetString($Memory.ToArray())
        return $FinalText | ConvertFrom-Json -ErrorAction Stop
    }
    finally {
        $Memory.Dispose()
        $Client.Close()
    }
}

function Assert-UemcpSuccess($Response, [string]$CommandType) {
    if (-not $Response -or $Response.status -ne "success") {
        $Message = if ($Response -and $Response.error) { $Response.error } else { "no response" }
        throw "$CommandType failed: $Message"
    }
}

$SmokeDir = Join-Path (Join-Path $RepoRoot ".uemcp.local") "asset-workflow-smoke"
New-Item -ItemType Directory -Force -Path $SmokeDir | Out-Null
$RunId = Get-Date -Format "yyyyMMdd-HHmmss"
$BeforePath = Join-Path $SmokeDir "$RunId-before.json"
$AfterPath = Join-Path $SmokeDir "$RunId-after.json"
$DiffPath = Join-Path $SmokeDir "$RunId-diff.json"

$LaunchedEditorPid = $null
try {
    $StartParams = @{
        Port = $Port
        StartupTimeoutSeconds = $StartupTimeoutSeconds
        ExpectedAssetRoots = @($ImportRoot)
        AllowHarnessProject = $true
        WaitForBridge = $true
        WaitForEditorReady = $true
    }
    if ($UeRoot) { $StartParams.UeRoot = $UeRoot }
    if ($ProjectPath) { $StartParams.ProjectPath = $ProjectPath }
    if ($PluginPath) { $StartParams.PluginPath = $PluginPath }
    if ($ProfileName) { $StartParams.ProfileName = $ProfileName }
    if ($ProfileDir) { $StartParams.ProfileDir = $ProfileDir }
    if ($SkipBuild) { $StartParams.SkipBuild = $true }
    if ($SkipLaunch) { $StartParams.SkipLaunch = $true }
    if ($SkipFab) { $StartParams.NoFabRequirement = $true }

    $StartOutput = & $StartScript @StartParams
    foreach ($Line in $StartOutput) {
        Write-Output $Line
        if ($Line -match "ASSET_WORKFLOW_STARTED pid=(\d+)") {
            $LaunchedEditorPid = [int]$Matches[1]
        }
    }

    if ($SkipLaunch) {
        Write-Output "ASSET_WORKFLOW_SMOKE_PREFLIGHT_ONLY"
        return
    }

    $Before = Invoke-UemcpBridgeCommand `
        -CommandType "asset_intake_snapshot" `
        -Params @{
            roots = @($ImportRoot)
            include_dependencies = $true
            include_referencers = $false
            include_tags = $true
            limit = 10000
        } `
        -TimeoutMilliseconds $CommandTimeoutMilliseconds
    Assert-UemcpSuccess $Before "asset_intake_snapshot"
    $Before.result | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $BeforePath -Encoding UTF8
    Write-Output "ASSET_WORKFLOW_SMOKE_SNAPSHOT_BEFORE path=$BeforePath"

    $Import = Invoke-UemcpBridgeCommand `
        -CommandType "asset_import_from_disk" `
        -Params @{
            source_files = @((Resolve-Path -LiteralPath $FixtureAssetPath).Path)
            destination_path = $ImportRoot
            replace_existing = $true
            save_imported_assets = $false
            dry_run = $false
        } `
        -TimeoutMilliseconds $CommandTimeoutMilliseconds
    Assert-UemcpSuccess $Import "asset_import_from_disk"
    Write-Output "ASSET_WORKFLOW_SMOKE_IMPORT imported=$(@($Import.result.imported_assets).Count)"

    $After = Invoke-UemcpBridgeCommand `
        -CommandType "asset_intake_snapshot" `
        -Params @{
            roots = @($ImportRoot)
            include_dependencies = $true
            include_referencers = $false
            include_tags = $true
            limit = 10000
        } `
        -TimeoutMilliseconds $CommandTimeoutMilliseconds
    Assert-UemcpSuccess $After "asset_intake_snapshot"
    $After.result | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $AfterPath -Encoding UTF8
    Write-Output "ASSET_WORKFLOW_SMOKE_SNAPSHOT_AFTER path=$AfterPath"

    $DiffScript = @'
import json
import sys
from pathlib import Path

from uemcp_asset_intake import diff_snapshots

before = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
after = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
print(json.dumps(diff_snapshots(before, after), indent=2, sort_keys=True))
'@
    $DiffJson = $DiffScript | uv --directory $PythonDir run python - $BeforePath $AfterPath
    if ($LASTEXITCODE -ne 0) {
        throw "asset_intake_diff smoke calculation failed with exit code $LASTEXITCODE"
    }
    $DiffJson | Set-Content -LiteralPath $DiffPath -Encoding UTF8
    $DiffObject = $DiffJson | ConvertFrom-Json
    Write-Output "ASSET_WORKFLOW_SMOKE_DIFF path=$DiffPath"

    if ($ExpectedActors.Count -gt 0) {
        $PlacementValidation = Invoke-UemcpBridgeCommand `
            -CommandType "asset_validate_level_placements" `
            -Params @{ expected_actors = $ExpectedActors } `
            -TimeoutMilliseconds $CommandTimeoutMilliseconds
        Assert-UemcpSuccess $PlacementValidation "asset_validate_level_placements"
        Write-Output "ASSET_WORKFLOW_SMOKE_PLACEMENTS all_present=$($PlacementValidation.result.all_present)"
    }
    else {
        Write-Output "ASSET_WORKFLOW_SMOKE_PLACEMENTS_SKIPPED tool=asset_validate_level_placements"
    }

    $AddedPackages = @($DiffObject.added | ForEach-Object { $_.package_name } | Where-Object { $_ })
    if (-not $KeepImportedAssets -and $AddedPackages.Count -gt 0) {
        foreach ($PackageName in $AddedPackages) {
            $Delete = Invoke-UemcpBridgeCommand `
                -CommandType "asset_delete" `
                -Params @{
                    asset_path = $PackageName
                    dry_run = $false
                } `
                -TimeoutMilliseconds $CommandTimeoutMilliseconds
            Assert-UemcpSuccess $Delete "asset_delete"
            Write-Output "ASSET_WORKFLOW_SMOKE_CLEANUP_DELETED package=$PackageName"
        }

        $Fixup = Invoke-UemcpBridgeCommand `
            -CommandType "asset_fixup_redirectors" `
            -Params @{
                roots = @($ImportRoot)
                dry_run = $false
            } `
            -TimeoutMilliseconds $CommandTimeoutMilliseconds
        Assert-UemcpSuccess $Fixup "asset_fixup_redirectors"
        Write-Output "ASSET_WORKFLOW_SMOKE_CLEANUP_REDIRECTORS count=$($Fixup.result.redirector_count)"
    }

    Write-Output "ASSET_WORKFLOW_SMOKE_OK"
}
finally {
    if ($CloseLaunchedEditor -and $LaunchedEditorPid) {
        $EditorProcess = Get-Process -Id $LaunchedEditorPid -ErrorAction SilentlyContinue
        if ($EditorProcess) {
            Stop-Process -Id $LaunchedEditorPid -Force
            Write-Output "ASSET_WORKFLOW_SMOKE_EDITOR_CLOSED pid=$LaunchedEditorPid"
        }
    }
}
