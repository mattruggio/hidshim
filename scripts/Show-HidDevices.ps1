#Requires -Version 5
<#
    Lists the HID devices on this machine as VID:PID, which is the format
    hid-shim.ini wants.

    This exists because the device you want to keep is specific to the machine.
    The shim hides every HID device except the ones named in hid-shim.ini, so
    the one thing you have to supply is your gamepad's id.

    Typical use when setting this up somewhere new:

        .\Show-HidDevices.ps1

    Add -Raw to get objects instead of a formatted report, which is what you
    want if you need to filter:

        .\Show-HidDevices.ps1 -Raw | Where-Object Name -match 'game controller'

    Find your gamepad in the output, then in hid-shim.ini set:

        AllowDevices=<the gamepad's VID:PID>

    That hides everything else from DirectInput without needing to work out
    which device is actually at fault.

    Hiding a device here does not disable it in Windows. It is hidden from
    one process, and only while that process is running.
#>

[CmdletBinding()]
param(
    # Show every HID device rather than only those that look like game
    # controllers and input peripherals.
    [switch] $All,

    # Emit the device rows as objects instead of a formatted report, so the
    # output can be filtered and scripted against.
    [switch] $Raw
)

$ErrorActionPreference = 'Stop'

function Get-VidPid {
    param([string] $InstanceId)

    $vid = $null; $pid_ = $null; $transport = 'other'

    # USB and the HID entries stacked on top of it: VID_1B1C&PID_2B02
    if ($InstanceId -match 'VID_([0-9A-Fa-f]{4})') { $vid  = $Matches[1].ToUpper(); $transport = 'usb' }
    if ($InstanceId -match 'PID_([0-9A-Fa-f]{4})') { $pid_ = $Matches[1].ToUpper() }

    # Bluetooth LE uses a different spelling entirely:
    #
    #   ..._DEV_VID&021B1C_PID&1BFB_REV&070B_F54C493741CE&COL03\...
    #
    # The two hex digits after VID& are the vendor id *source*, 01 for a
    # Bluetooth SIG assigned id and 02 for a USB-IF one, and the four after
    # that are the vendor id. Matching only VID_ misses these completely, which
    # means a wireless keyboard or mouse never appears in this list even though
    # DirectInput still enumerates it and the shim still filters it. That is
    # exactly the device most likely to be at fault.
    if (-not $vid  -and $InstanceId -match 'VID&[0-9A-Fa-f]{2}([0-9A-Fa-f]{4})') {
        $vid = $Matches[1].ToUpper(); $transport = 'bluetooth'
    }
    if (-not $pid_ -and $InstanceId -match 'PID&([0-9A-Fa-f]{4})') { $pid_ = $Matches[1].ToUpper() }

    # Virtual HID buses, such as the one iCUE installs, carry no vendor id at
    # all. They cannot be targeted by VID:PID, so the shim cannot filter them
    # and they are reported here only so you know they exist.
    if (-not $vid) {
        if ($InstanceId -match 'VIRTUALDEVICE|CORSAIRBUS') {
            return [pscustomobject]@{ Vid = $null; Pid = $null; VidPid = '(virtual)'; Transport = 'virtual' }
        }
        return $null
    }

    [pscustomobject]@{
        Vid       = $vid
        Pid       = $pid_
        VidPid    = if ($pid_) { "$vid`:$pid_" } else { $vid }
        Transport = $transport
    }
}

$devices = Get-PnpDevice -Class HIDClass -ErrorAction SilentlyContinue

if (-not $devices) {
    Write-Warning 'No HID devices found.'
    return
}

$rows = foreach ($d in $devices) {
    $ids = Get-VidPid $d.InstanceId
    if (-not $ids) { continue }

    [pscustomobject]@{
        VidPid      = $ids.VidPid
        Vendor      = $ids.Vid
        Transport   = $ids.Transport
        Status      = $d.Status
        Name        = $d.FriendlyName
    }
}

if (-not $All) {
    # Most HID entries are extra collections on the same physical device. The
    # interesting ones for the shim are the peripherals you can name.
    $rows = $rows | Where-Object {
        $_.Name -and $_.Name -notmatch 'HID-compliant (consumer|system|vendor|device)'
    }
}

if ($Raw) {
    $rows | Sort-Object Vendor, Name
    return
}

"HID devices on this machine"
""
$rows |
    Sort-Object Vendor, Name |
    Format-Table -AutoSize VidPid, Transport, Status, Name

""
"Grouped by vendor, which is what a bare entry in hid-shim.ini matches:"
""
$rows |
    Where-Object { $_.Vendor } |
    Group-Object Vendor |
    Sort-Object Count -Descending |
    ForEach-Object {
        "  {0}  {1,2} device(s)  {2}" -f $_.Name, $_.Count, (($_.Group.Name | Select-Object -First 1))
    }

if ($rows | Where-Object { $_.Transport -eq 'bluetooth' }) {
    ""
    "Some of these are connected over Bluetooth. They are filtered by VID:PID"
    "exactly like a USB device, so nothing about the policy changes, but be aware"
    "that unplugging a wireless receiver is not the same as unpairing."
}

if ($rows | Where-Object { $_.Transport -eq 'virtual' }) {
    ""
    "A virtual HID bus is present, shown as (virtual). It exposes no vendor id, so"
    "it cannot be named in hid-shim.ini. It gets hidden anyway, because anything"
    "not on the allow list is hidden."
}

""
"A vendor with a lot of entries is the usual suspect: each collection is more"
"work for DirectInput's report descriptor parser."
""
"Name the pad you want to keep:   AllowDevices=<VID:PID>"
"Everything you do not name is hidden from the target."
