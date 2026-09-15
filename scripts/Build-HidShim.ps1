#Requires -Version 5
<#
    Builds the hid.dll shim and checks that it is a valid drop in replacement
    for the system hid.dll.

    The shim is 32 bit, because the applications that need it are. The
    toolchain is LLVM-MinGW, which ships an i686 target and its own hidsdi.h,
    so nothing else needs installing.

    Export names matter more here than they would for most shims. DirectInput
    resolves every HID function by name with GetProcAddress, so a missing or
    decorated export does not fail loudly, it just makes DirectInput think the
    function is unavailable. This script therefore compares the finished export
    list against the real hid.dll and fails the build on any difference.

    Use Use-HidShim.ps1 to install or remove the result.
#>

[CmdletBinding()]
param(
    [string] $SourceDir,
    [switch] $DebugBuild
)

$ErrorActionPreference = 'Stop'

# Resolved here, not as a param default. In an advanced script under Windows
# PowerShell 5.1, $PSScriptRoot is empty while param defaults are evaluated if
# the script was launched with -File.
if (-not $SourceDir) { $SourceDir = Join-Path $PSScriptRoot '..\src' }

$source = Join-Path $SourceDir 'hid-shim.c'
$output = Join-Path $SourceDir 'hid.dll'
$realHid = Join-Path $env:WINDIR 'SysWOW64\hid.dll'

if (-not (Test-Path $source)) { throw "Missing source: $source" }

$gcc = Get-Command 'i686-w64-mingw32-gcc' -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty Source -First 1

if (-not $gcc) {
    $gcc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" `
        -Recurse -Filter 'i686-w64-mingw32-gcc.exe' -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}

if (-not $gcc) {
    throw 'No i686-w64-mingw32-gcc found. Install it with: winget install MartinStorsjo.LLVM-MinGW.MSVCRT'
}

Write-Host "Compiler: $gcc"

$flags = @(
    '-shared',
    '-o', $output,
    $source,
    # Exported stdcall functions keep an @nn suffix by default, which would not
    # match the names DirectInput asks GetProcAddress for.
    '-Wl,--kill-at',
    '-Wall', '-Wno-dll-attribute-on-redeclaration',
    '-static-libgcc',
    '-lkernel32'
)
# The forwarders are naked jumps, so optimisation must not reorder or inline
# around them. -O2 is still fine, but no frame pointer games.
$flags += if ($DebugBuild) { '-O0', '-g' } else { '-O2' }

Remove-Item $output -Force -ErrorAction SilentlyContinue

& $gcc @flags 2>&1 | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw "Compilation failed with exit code $LASTEXITCODE" }
if (-not (Test-Path $output)) { throw 'Compilation reported success but produced no DLL' }

Write-Host "Built: $output ($((Get-Item $output).Length) bytes)"

function Get-PeExports {
    param([string] $Path)

    $b  = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($b, 0x3c)

    $machine = [BitConverter]::ToUInt16($b, $pe + 4)
    $nsec    = [BitConverter]::ToUInt16($b, $pe + 6)
    $optsz   = [BitConverter]::ToUInt16($b, $pe + 20)
    $secT    = $pe + 24 + $optsz

    $secs = @()
    for ($i = 0; $i -lt $nsec; $i++) {
        $o = $secT + ($i * 40)
        $secs += [pscustomobject]@{
            va = [BitConverter]::ToUInt32($b, $o + 12)
            rs = [BitConverter]::ToUInt32($b, $o + 16)
            ro = [BitConverter]::ToUInt32($b, $o + 20)
        }
    }

    function Convert-RvaToOffset {
        param($rva)
        foreach ($s in $secs) {
            if ($rva -ge $s.va -and $rva -lt ($s.va + $s.rs)) {
                return $s.ro + ($rva - $s.va)
            }
        }
        return -1
    }

    $expRva = [BitConverter]::ToUInt32($b, $pe + 24 + 96)
    $names  = @()

    if ($expRva -ne 0) {
        $e = Convert-RvaToOffset $expRva
        $cnt     = [BitConverter]::ToUInt32($b, $e + 24)
        $nameRva = [BitConverter]::ToUInt32($b, $e + 32)
        $nt      = Convert-RvaToOffset $nameRva

        for ($i = 0; $i -lt $cnt; $i++) {
            $no  = Convert-RvaToOffset ([BitConverter]::ToUInt32($b, $nt + ($i * 4)))
            $end = $no
            while ($b[$end] -ne 0) { $end++ }
            $names += [Text.Encoding]::ASCII.GetString($b, $no, $end - $no)
        }
    }

    [pscustomobject]@{
        Machine = '0x{0:X4}' -f $machine
        Is32Bit = ($machine -eq 0x14c)
        Exports = $names
    }
}

$mine = Get-PeExports -Path $output
$real = Get-PeExports -Path $realHid

Write-Host ''
Write-Host "Architecture : $($mine.Machine) $(if ($mine.Is32Bit) { '(i386, correct)' } else { 'WRONG, must be i386' })"
Write-Host "Exports      : $($mine.Exports.Count) (real hid.dll has $($real.Exports.Count))"

if (-not $mine.Is32Bit) { throw 'The shim is not a 32 bit DLL, so a 32 bit target cannot load it.' }

# Anything the real DLL exports and this one does not is a function DirectInput
# could ask for and be told does not exist.
$missing = $real.Exports | Where-Object { $mine.Exports -notcontains $_ }
if ($missing) {
    throw "Missing exports that the real hid.dll provides: $($missing -join ', ')"
}

$extra = $mine.Exports | Where-Object { $real.Exports -notcontains $_ }
if ($extra) {
    Write-Warning "Exported but absent from the real hid.dll: $($extra -join ', ')"
}

# The two that carry the filter. If these were decorated or dropped the shim
# would load and do nothing, which is the failure mode hardest to notice.
foreach ($fn in 'HidD_GetAttributes', 'HidD_GetPreparsedData') {
    if ($mine.Exports -notcontains $fn) { throw "The filter export $fn is missing." }
}

Write-Host 'All real hid.dll exports are present, including both filtered ones.'

# The export table proves the names are there. It cannot prove the forwarders
# execute, because they are naked jumps, so a mistake is a crash rather than a
# wrong answer. Nor can it prove the filter reads your ini. The smoke test
# loads the shim and enumerates HID devices the way DirectInput does, which
# answers both before the game is involved.
$smokeSrc = Join-Path $SourceDir 'test-hid-shim.c'
if (Test-Path $smokeSrc) {
    Write-Host ''
    Write-Host 'Running the smoke test...'

    $smokeExe = Join-Path $SourceDir 'test-hid-shim.exe'
    & $gcc -o $smokeExe $smokeSrc -lsetupapi
    if ($LASTEXITCODE -ne 0) { throw 'The smoke test did not compile.' }

    try {
        Push-Location $SourceDir
        & $smokeExe
        $smokeCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
        Remove-Item $smokeExe -Force -ErrorAction SilentlyContinue
    }

    if ($smokeCode -ne 0) { throw "The smoke test failed (exit $smokeCode)." }
}

Write-Host ''
Write-Host 'Build verified.' -ForegroundColor Green
