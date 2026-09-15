#Requires -Version 5
<#
    Installs or removes the hid.dll shim in a target application's directory.

    DirectInput loads hid.dll at runtime by bare name, and hid.dll is not in the
    KnownDLLs registry key, so the loader searches the application directory
    first and finds this copy. That puts the shim underneath DirectInput rather
    than above it, which is the whole point: it can hide devices before
    DirectInput parses their report descriptors.

    It changes nothing outside the target process. The keyboard keeps working
    everywhere else, and a crash cannot leave the machine in a broken state.

    Build it first with Build-HidShim.ps1.

      -Install   copy the shim and its configuration into the target directory
      -Remove    delete both, so the target uses the system hid.dll again
      -Status    report what is currently there

    -TargetDir is the directory holding the executable you want to shim, not
    the executable itself. Removing is always safe: without a hid.dll beside it
    the target behaves exactly as it did before.

        .\Use-HidShim.ps1 -Status  -TargetDir 'C:\Program Files (x86)\Some Game'
        .\Use-HidShim.ps1 -Install -TargetDir 'C:\Program Files (x86)\Some Game'
#>

[CmdletBinding(DefaultParameterSetName = 'Status')]
param(
    [Parameter(ParameterSetName = 'Install')] [switch] $Install,
    [Parameter(ParameterSetName = 'Remove')]  [switch] $Remove,
    [Parameter(ParameterSetName = 'Status')]  [switch] $Status,

    [Parameter(Mandatory)]
    [string] $TargetDir,

    [string] $SourceDir
)

$ErrorActionPreference = 'Stop'

# Resolved here, not as a param default. In an advanced script under Windows
# PowerShell 5.1, $PSScriptRoot is empty while param defaults are evaluated if
# the script was launched with -File, which is how this script re-launches
# itself elevated.
if (-not $SourceDir) { $SourceDir = Join-Path $PSScriptRoot '..\src' }

if (-not (Test-Path $TargetDir)) { throw "No such directory: $TargetDir" }

$targetDll = Join-Path $TargetDir 'hid.dll'
$targetIni = Join-Path $TargetDir 'hid-shim.ini'
$srcDll    = Join-Path $SourceDir 'hid.dll'
$srcIni    = Join-Path $SourceDir 'hid-shim.ini'

# Hash via .NET rather than Get-FileHash. The elevated child process does not
# reliably have Microsoft.PowerShell.Utility available.
function Get-Sha256 {
    param([string] $Path)

    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try {
            return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '')
        }
        finally { $stream.Dispose() }
    }
    finally { $sha.Dispose() }
}

function Get-LogPath {
    # Mirrors the DLL's own resolution: an explicit Path in the ini wins,
    # otherwise the default under LOCALAPPDATA.
    $ini = if (Test-Path $targetIni) { $targetIni } elseif (Test-Path $srcIni) { $srcIni } else { $null }

    if ($ini) {
        $line = Select-String -Path $ini -Pattern '^\s*Path\s*=\s*(\S.*)$' |
            Select-Object -Last 1
        if ($line) { return $line.Matches[0].Groups[1].Value.Trim() }
    }

    Join-Path $env:LOCALAPPDATA 'hidshim\hid-shim.log'
}

function Show-Status {
    $logFile = Get-LogPath

    Write-Host ''
    Write-Host "Target directory : $TargetDir"

    if (Test-Path $targetDll) {
        $item = Get-Item $targetDll
        Write-Host "hid.dll          : present, $($item.Length) bytes, built $($item.LastWriteTime)" -ForegroundColor Cyan

        if (Test-Path $srcDll) {
            if ((Get-Sha256 -Path $targetDll) -eq (Get-Sha256 -Path $srcDll)) {
                Write-Host '                   matches the built copy'
            }
            else {
                Write-Warning 'The installed shim differs from the built copy. Reinstall to update it.'
            }
        }
    }
    else {
        Write-Host 'hid.dll          : absent, the target will use the system hid.dll'
    }

    Write-Host "config           : $(if (Test-Path $targetIni) { 'present' } else { 'absent, built in defaults apply' })"
    Write-Host "log              : $(if (Test-Path $logFile) { $logFile } else { "$logFile (not written yet)" })"
    Write-Host ''
}

if ($PSCmdlet.ParameterSetName -eq 'Status' -or
    (-not $Install -and -not $Remove)) {
    Show-Status
    return
}

$isElevated = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isElevated) {
    Write-Host 'Writing to the target directory may need elevation. Accept the UAC prompt.'
    $argList = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', "`"$PSCommandPath`"",
        $(if ($Install) { '-Install' } else { '-Remove' }),
        '-TargetDir', "`"$TargetDir`"",
        '-SourceDir', "`"$SourceDir`""
    )
    Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait
    return
}

# Overwriting a DLL that is currently mapped into a running process fails in
# confusing ways, so refuse up front. Any process running from the target
# directory counts, whatever it is called.
$running = Get-Process -ErrorAction SilentlyContinue | Where-Object {
    try { $_.Path -and $_.Path.StartsWith($TargetDir, [StringComparison]::OrdinalIgnoreCase) }
    catch { $false }
}
if ($running) {
    throw "Running from the target directory: $(($running.Name | Sort-Object -Unique) -join ', '). Close it before changing the shim."
}

if ($Install) {
    foreach ($required in @($srcDll, $srcIni)) {
        if (-not (Test-Path $required)) {
            throw "Missing: $required. Run Build-HidShim.ps1 first."
        }
    }

    # Refuse to overwrite a hid.dll this script did not build. A real hid.dll
    # has no business being in an application directory, so anything
    # unrecognised here is more likely a mistaken path than something to
    # clobber.
    if (Test-Path $targetDll) {
        $existing = (Get-Sha256 -Path $targetDll)
        $built    = (Get-Sha256 -Path $srcDll)
        $known    = Join-Path $SourceDir 'installed.sha256'
        $previous = if (Test-Path $known) { (Get-Content $known -Raw).Trim() } else { '' }

        if ($existing -ne $built -and $existing -ne $previous) {
            throw "An unrecognised hid.dll is already in $TargetDir. Remove it by hand if it is not needed."
        }
    }

    Copy-Item $srcDll $targetDll -Force
    Copy-Item $srcIni $targetIni -Force
    (Get-Sha256 -Path $targetDll) |
        Set-Content (Join-Path $SourceDir 'installed.sha256') -Encoding ASCII

    Write-Host 'Installed the hid.dll shim.'
    Write-Host 'Start the target, then check the log names the devices you expect:'
    Write-Host "  Get-Content `"$(Get-LogPath)`" -Tail 20"
}

if ($Remove) {
    Remove-Item $targetDll -Force -ErrorAction SilentlyContinue
    Remove-Item $targetIni -Force -ErrorAction SilentlyContinue
    Write-Host 'Removed the hid.dll shim. The target will use the system hid.dll.'
    Write-Host 'Whatever the shim was working around will come back.'
}

Show-Status
