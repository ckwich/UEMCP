param(
    [string]$UeRoot = $env:UEMCP_UE_ROOT,
    [string]$ProjectPath,
    [string]$PluginPath,
    [string]$ProfileName = $env:UEMCP_PROFILE_NAME,
    [string]$ProfileDir = $env:UEMCP_PROFILE_DIR,
    [string[]]$ExpectedAssetRoots = @(),
    [int]$Port = 55557,
    [int]$StartupTimeoutSeconds = 300,
    [switch]$SkipBuild,
    [switch]$SkipLaunch,
    [switch]$AllowHarnessProject,
    [switch]$NoFabRequirement,
    [switch]$WaitForBridge,
    [switch]$WaitForEditorReady
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
$RepoRoot = Split-Path -Parent $ScriptDir
$HarnessProjectPath = Join-Path (Join-Path $RepoRoot "MCPGameProject") "MCPGameProject.uproject"
$DefaultPluginPath = Join-Path (Join-Path (Join-Path (Join-Path $RepoRoot "MCPGameProject") "Plugins") "UnrealMCP") "UnrealMCP.uplugin"

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
    if (-not $AllowHarnessProject) {
        throw "Interactive asset workflows require an explicit consuming project -ProjectPath. Pass -AllowHarnessProject only for local script testing."
    }

    $ProjectPath = $HarnessProjectPath
}

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

foreach ($PathToCheck in @($EditorPath, $BuildPath, $ProjectPath)) {
    if (-not (Test-Path -LiteralPath $PathToCheck)) {
        throw "Required path not found: $PathToCheck"
    }
}

$ResolvedProjectPath = (Resolve-Path -LiteralPath $ProjectPath).Path
$ResolvedHarnessProjectPath = (Resolve-Path -LiteralPath $HarnessProjectPath).Path
$ResolvedPluginPath = $null
$ResolvedProfileDir = $null

if ($ResolvedProjectPath -eq $ResolvedHarnessProjectPath -and -not $AllowHarnessProject) {
    throw "Interactive asset workflows must target a consuming project, not MCPGameProject. Pass -AllowHarnessProject only for local script testing."
}

if (-not $ProfileName) {
    $ProfileName = "default"
}

if ($PluginPath) {
    if (-not (Test-Path -LiteralPath $PluginPath -PathType Leaf)) {
        throw "Plugin path not found: $PluginPath"
    }

    $ResolvedPluginPath = (Resolve-Path -LiteralPath $PluginPath).Path
}
else {
    $ProjectPluginPath = Join-Path (Join-Path (Join-Path (Split-Path -Parent $ResolvedProjectPath) "Plugins") "UnrealMCP") "UnrealMCP.uplugin"
    if (-not (Test-Path -LiteralPath $ProjectPluginPath -PathType Leaf) -and $ResolvedProjectPath -ne $ResolvedHarnessProjectPath) {
        if (Test-Path -LiteralPath $DefaultPluginPath -PathType Leaf) {
            $ResolvedPluginPath = (Resolve-Path -LiteralPath $DefaultPluginPath).Path
        }
        else {
            throw "UEMCP plugin is not attached to this project and the repo plugin was not found."
        }
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

        return $null
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
        & $BuildPath `
            "$(Get-UemcpEditorTargetName $ProjectFilePath)" `
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

function Test-FabAvailability([string]$ProjectFilePath) {
    $FabPluginPath = Join-Path $UeRoot "Engine/Plugins/Fab/Fab.uplugin"
    if (-not (Test-Path -LiteralPath $FabPluginPath -PathType Leaf)) {
        throw "Fab plugin was not found under this Unreal Engine install: $FabPluginPath"
    }

    $ProjectJson = Get-Content -LiteralPath $ProjectFilePath -Raw | ConvertFrom-Json
    $FabPlugin = @($ProjectJson.Plugins) | Where-Object { $_.Name -eq "Fab" } | Select-Object -First 1
    if ($FabPlugin -and $FabPlugin.Enabled -eq $false) {
        throw "Fab is explicitly disabled in this project. Enable Fab in the consuming project before starting the interactive asset workflow."
    }

    $ProjectFabState = if ($FabPlugin) { "configured" } else { "engine-default" }
    Write-Output "FAB_CHECK_OK installed=$FabPluginPath project_state=$ProjectFabState"
}

if (-not $NoFabRequirement) {
    Test-FabAvailability $ResolvedProjectPath
}

$ResolvedPluginOwnerProjectPath = $null
if ($ResolvedPluginPath) {
    $PluginOwnerProjectPath = Find-PluginOwnerProjectPath $ResolvedPluginPath
    if ($PluginOwnerProjectPath) {
        $ResolvedPluginOwnerProjectPath = (Resolve-Path -LiteralPath $PluginOwnerProjectPath).Path
    }
}

if (-not $SkipBuild) {
    if ($ResolvedPluginOwnerProjectPath -and $ResolvedPluginOwnerProjectPath -ne $ResolvedProjectPath) {
        Invoke-UemcpEditorBuild $ResolvedPluginOwnerProjectPath "Interactive asset workflow plugin owner build"
    }

    Invoke-UemcpEditorBuild $ResolvedProjectPath "Interactive asset workflow target build"
}

if ($ResolvedProfileDir) {
    $env:UEMCP_PROFILE_DIR = $ResolvedProfileDir
    Write-Output "Using UEMCP project profile directory: $ResolvedProfileDir"
}

if ($ProfileName) {
    $env:UEMCP_PROFILE_NAME = $ProfileName
}

Write-Output "ASSET_WORKFLOW_PREFLIGHT_OK project=$ResolvedProjectPath profile=$ProfileName"
if ($ExpectedAssetRoots.Count -gt 0) {
    $ExpectedAssetRootText = ($ExpectedAssetRoots | Where-Object { $_ } | ForEach-Object { $_.Trim() }) -join ","
    Write-Output "ASSET_WORKFLOW_EXPECTED_ROOTS roots=$ExpectedAssetRootText"
}

if ($SkipLaunch) {
    return
}

$EditorArgs = @(
    $ResolvedProjectPath,
    "-log",
    "-nosplash",
    "-nop4"
)

if ($ResolvedPluginPath) {
    $EditorArgs += "-PLUGIN=$ResolvedPluginPath"
}

Write-Output "Launching interactive Unreal Editor for asset workflow: $ResolvedProjectPath"
$LaunchedEditor = Start-Process -FilePath $EditorPath -ArgumentList $EditorArgs -PassThru
Write-Output "ASSET_WORKFLOW_STARTED pid=$($LaunchedEditor.Id) port=$Port"

if ($WaitForEditorReady) {
    $WaitForBridge = $true
}

if ($WaitForBridge) {
    $BridgeDeadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
    while ((Get-Date) -lt $BridgeDeadline) {
        if (Test-UemcpBridgeListener) {
            Write-Output "ASSET_WORKFLOW_BRIDGE_READY port=$Port"
            break
        }

        if ($LaunchedEditor.HasExited) {
            throw "Unreal Editor exited before the UEMCP bridge opened."
        }

        Start-Sleep -Seconds 1
    }

    if (-not (Test-UemcpBridgeListener)) {
        throw "Timed out waiting for UEMCP bridge on 127.0.0.1:$Port."
    }
}

if ($WaitForEditorReady) {
    $EditorReadyDeadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
    $EditorReadyResponse = $null
    while ((Get-Date) -lt $EditorReadyDeadline) {
        $EditorReadyResponse = Invoke-UemcpBridgeCommand `
            -CommandType "get_editor_status" `
            -Params @{} `
            -TimeoutMilliseconds 5000
        if ($EditorReadyResponse -and $EditorReadyResponse.status -eq "success") {
            Write-Output "ASSET_WORKFLOW_EDITOR_READY port=$Port"
            break
        }

        if ($LaunchedEditor.HasExited) {
            throw "Unreal Editor exited before editor-backed UEMCP commands became ready."
        }

        Start-Sleep -Seconds 2
    }

    if (-not $EditorReadyResponse -or $EditorReadyResponse.status -ne "success") {
        $LastEditorReadyError = if ($EditorReadyResponse) { $EditorReadyResponse.error } else { "no response" }
        throw "Timed out waiting for editor-backed UEMCP commands. Last status: $LastEditorReadyError"
    }
}
