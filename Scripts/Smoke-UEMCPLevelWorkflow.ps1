param(
    [string]$UeRoot = $env:UEMCP_UE_ROOT,
    [string]$ProjectPath,
    [string]$PluginPath,
    [string]$ProfileName = $env:UEMCP_PROFILE_NAME,
    [string]$ProfileDir = $env:UEMCP_PROFILE_DIR,
    [string]$TargetMap = "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke",
    [int]$Port = 55557,
    [int]$StartupTimeoutSeconds = 300,
    [int]$CommandTimeoutMilliseconds = 30000,
    [switch]$SkipBuild,
    [switch]$SkipLaunch,
    [switch]$KeepSmokeMap,
    [switch]$CloseLaunchedEditor
)

# Example: pwsh -NoProfile -ExecutionPolicy Bypass -File ./Scripts/Smoke-UEMCPLevelWorkflow.ps1 -CloseLaunchedEditor

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
$RepoRoot = Split-Path -Parent $ScriptDir
$StartScript = Join-Path $ScriptDir "Start-UEMCPInteractiveAssetWorkflow.ps1"

if (-not (Test-Path -LiteralPath $StartScript)) {
    throw "Required level workflow smoke path not found: $StartScript"
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

$ActorName = "UEMCP_LevelSmoke_PointLight"
$ConstructionOperations = @(
    @{
        op = "ensure_actor"
        actor_name = $ActorName
        actor_class = "/Script/Engine.PointLight"
        label = "UEMCP Level Smoke Point Light"
        folder_path = "UEMCP/Smoke"
        location = @(0.0, 0.0, 180.0)
        rotation = @(0.0, 0.0, 0.0)
        scale = @(1.0, 1.0, 1.0)
        tags = @("UEMCPSmoke")
    }
)
$ExpectedActors = @(
    @{
        actor_name = $ActorName
        class = "PointLight"
        folder_path = "UEMCP/Smoke"
        tags = @("UEMCPSmoke")
        location = @(0.0, 0.0, 180.0)
    }
)

$LaunchedEditorPid = $null
try {
    $StartParams = @{
        Port = $Port
        StartupTimeoutSeconds = $StartupTimeoutSeconds
        ExpectedAssetRoots = @("/Game/UEMCP/Smoke")
        AllowHarnessProject = $true
        WaitForBridge = $true
        WaitForEditorReady = $true
        NoFabRequirement = $true
    }
    if ($UeRoot) { $StartParams.UeRoot = $UeRoot }
    if ($ProjectPath) { $StartParams.ProjectPath = $ProjectPath }
    if ($PluginPath) { $StartParams.PluginPath = $PluginPath }
    if ($ProfileName) { $StartParams.ProfileName = $ProfileName }
    if ($ProfileDir) { $StartParams.ProfileDir = $ProfileDir }
    if ($SkipBuild) { $StartParams.SkipBuild = $true }
    if ($SkipLaunch) { $StartParams.SkipLaunch = $true }

    $StartOutput = & $StartScript @StartParams
    foreach ($Line in $StartOutput) {
        Write-Output $Line
        if ($Line -match "ASSET_WORKFLOW_STARTED pid=(\d+)") {
            $LaunchedEditorPid = [int]$Matches[1]
        }
    }

    if ($SkipLaunch) {
        Write-Output "LEVEL_WORKFLOW_SMOKE_PREFLIGHT_ONLY"
        return
    }

    Write-Output "LEVEL_WORKFLOW_BRIDGE_READY"
    Write-Output "LEVEL_WORKFLOW_EDITOR_READY"

    $Create = Invoke-UemcpBridgeCommand `
        -CommandType "level_create" `
        -Params @{
            package_path = $TargetMap
            save_existing = $false
            save_new_level = $true
            fail_if_exists = $false
            dry_run = $false
        } `
        -TimeoutMilliseconds $CommandTimeoutMilliseconds
    Assert-UemcpSuccess $Create "level_create"
    Write-Output "LEVEL_WORKFLOW_SMOKE_CREATED package=$TargetMap"

    $Apply = Invoke-UemcpBridgeCommand `
        -CommandType "level_apply_construction_plan" `
        -Params @{
            target_map = $TargetMap
            open_level = $true
            create_if_missing = $false
            save_level = $false
            dry_run = $false
            operations = $ConstructionOperations
        } `
        -TimeoutMilliseconds $CommandTimeoutMilliseconds
    Assert-UemcpSuccess $Apply "level_apply_construction_plan"
    Write-Output "LEVEL_WORKFLOW_SMOKE_APPLIED created=$(@($Apply.result.created_actors).Count)"

    $Validation = Invoke-UemcpBridgeCommand `
        -CommandType "level_validate_construction" `
        -Params @{
            target_map = $TargetMap
            expected_actors = $ExpectedActors
            location_tolerance = 0.1
        } `
        -TimeoutMilliseconds $CommandTimeoutMilliseconds
    Assert-UemcpSuccess $Validation "level_validate_construction"
    if (-not $Validation.result.passed) {
        throw "level_validate_construction did not pass"
    }
    Write-Output "LEVEL_WORKFLOW_SMOKE_VALIDATED passed=$($Validation.result.passed)"

    $Save = Invoke-UemcpBridgeCommand `
        -CommandType "level_save" `
        -Params @{
            package_path = $TargetMap
            only_if_dirty = $true
            include_external_actor_packages = $true
        } `
        -TimeoutMilliseconds $CommandTimeoutMilliseconds
    Assert-UemcpSuccess $Save "level_save"
    Write-Output "LEVEL_WORKFLOW_SMOKE_SAVED packages=$(@($Save.result.saved_packages).Count)"

    if (-not $KeepSmokeMap) {
        $Blank = Invoke-UemcpBridgeCommand `
            -CommandType "level_create" `
            -Params @{
                package_path = "/Game/UEMCP/Smoke/Maps/L_UEMCP_LevelSmoke_CleanupBlank"
                save_existing = $false
                save_new_level = $false
                fail_if_exists = $false
                dry_run = $false
            } `
            -TimeoutMilliseconds $CommandTimeoutMilliseconds
        Assert-UemcpSuccess $Blank "level_create"

        $Delete = Invoke-UemcpBridgeCommand `
            -CommandType "asset_delete" `
            -Params @{
                asset_path = $TargetMap
                dry_run = $false
            } `
            -TimeoutMilliseconds $CommandTimeoutMilliseconds
        Assert-UemcpSuccess $Delete "asset_delete"
        Write-Output "LEVEL_WORKFLOW_SMOKE_CLEANUP_DELETED package=$TargetMap"

        $Fixup = Invoke-UemcpBridgeCommand `
            -CommandType "asset_fixup_redirectors" `
            -Params @{
                roots = @("/Game/UEMCP/Smoke")
                dry_run = $false
            } `
            -TimeoutMilliseconds $CommandTimeoutMilliseconds
        Assert-UemcpSuccess $Fixup "asset_fixup_redirectors"
        Write-Output "LEVEL_WORKFLOW_SMOKE_CLEANUP_REDIRECTORS count=$($Fixup.result.redirector_count)"
    }

    Write-Output "LEVEL_WORKFLOW_SMOKE_OK"
}
finally {
    if ($CloseLaunchedEditor -and $LaunchedEditorPid) {
        $EditorProcess = Get-Process -Id $LaunchedEditorPid -ErrorAction SilentlyContinue
        if ($EditorProcess) {
            Stop-Process -Id $LaunchedEditorPid -Force
            Write-Output "LEVEL_WORKFLOW_SMOKE_EDITOR_CLOSED pid=$LaunchedEditorPid"
        }
    }
}
