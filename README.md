# hidshim

[![build](https://github.com/mattruggio/hidshim/actions/workflows/build.yml/badge.svg)](https://github.com/mattruggio/hidshim/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/mattruggio/hidshim?label=release&sort=semver)](https://github.com/mattruggio/hidshim/releases/latest)

Hide HID devices from a single Windows process.

This is a proxy `hid.dll`. Drop it beside an executable, list the devices that executable is allowed to see, and every other HID device becomes invisible to it. Nothing is installed: no driver, no service, nothing in the background. Uninstalling is deleting two files.

It exists because a modern Corsair keyboard presents fifteen HID collections, DirectInput's report descriptor parser cannot cope with that many, and **NBA Live 2005** died a few seconds after launch with a corrupted heap every single time. Not the GPU, not the OS version, not the age of the game. Hiding everything except the gamepad fixed it outright, in 2026, on a Core i5-13600K with an RTX 2060.

Any DirectInput era application can hit this, and RGB software, virtual HID buses, wireless receivers and composite devices make it likelier every year.

**32 bit processes only.** See [Limits](#limits).

---

## 1. Get it

Download `hidshim-<version>-i386.zip` from the [latest release](https://github.com/mattruggio/hidshim/releases/latest) and extract it anywhere. Then open PowerShell in the extracted folder, the one containing `scripts\` and `src\`:

```powershell
cd C:\path\to\hidshim-<version>-i386
```

**Every command below is written to run from there, not from inside `scripts\`.** The scripts themselves resolve their own paths, so they work from any directory, but the `.\scripts\Something.ps1` prefix only resolves from the folder root.

Windows refuses to run scripts at all by default, and separately marks anything extracted from a downloaded zip as untrusted. Clear both, for this window only:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
Get-ChildItem -Recurse | Unblock-File
```

`-Scope Process` lasts until you close that window, needs no admin rights, and changes nothing permanently. Skip it and you get `running scripts is disabled on this system`. Skip `Unblock-File` and the scripts stay blocked even after the policy is relaxed, because the mark is a separate mechanism.

It is an unsigned DLL that proxies a system DLL, which is structurally indistinguishable from malware. Verify it rather than trusting it:

```powershell
gh attestation verify src\hid.dll --repo mattruggio/hidshim
```

That proves the binary came out of the build workflow, in this repository, from a specific commit. `SHA256SUMS.txt` is published alongside if you prefer comparing hashes by hand.

Or [build it yourself](#building), which takes about a minute and removes the question entirely.

## 2. Find your device

```powershell
.\scripts\Show-HidDevices.ps1
```

Prints every HID device as `VID:PID`, grouped by vendor, which is the format the config wants. It catches Bluetooth LE devices too, which spell their ids differently (`VID&02046D` rather than `VID_046D`) and are otherwise easy to miss.

A vendor with a lot of entries is the usual suspect. Each collection is more work for the parser.

## 3. Configure

Edit `src\hid-shim.ini` and name what you want kept:

```ini
[Filter]
Enabled=1
AllowDevices=046D:C216
```

That is a Logitech F310 gamepad. Comma separate for more than one. A bare `046D` covers every product from that vendor, but prefer the full `VID:PID`, since the company that made your gamepad may well have made a keyboard you want hidden.

There is no block list, deliberately. A rule can only name a vendor id, and a virtual HID bus such as the one iCUE installs does not have one, so no block list can even express it. An allow list also asks the easier question: you know which device you want, not which one is at fault.

An empty allow list hides everything, which cannot crash. That is the safe default rather than an unconfigured one.

**If you do not know what to allow yet,** set `Enabled=0`. The shim forwards everything untouched and becomes a pure logger, so you can watch which devices the target opens and in what order.

Your keyboard and mouse keep working regardless. DirectInput supplies those from its system keyboard and mouse objects, which never pass through this enumeration.

## 4. Install

Point it at the directory holding the executable, not the executable itself:

```powershell
.\scripts\Use-HidShim.ps1 -Install -TargetDir 'C:\Program Files (x86)\EA SPORTS\NBA LIVE 2005'
```

Accept the UAC prompt. The script self-elevates because the target is usually under `Program Files` and Windows cannot escalate in place.

It copies two files, `hid.dll` and `hid-shim.ini`, and nothing else. It refuses to overwrite a `hid.dll` it did not build, on the grounds that a real one has no business being in an application directory, so an unrecognised one is more likely a mistyped path.

```powershell
.\scripts\Use-HidShim.ps1 -Status -TargetDir '...'
.\scripts\Use-HidShim.ps1 -Remove -TargetDir '...'
```

## 5. Verify

Start the target, then:

```powershell
Get-Content "$env:LOCALAPPDATA\hidshim\hid-shim.log" -Tail 20
```

```
[13:07:35] === hid shim loaded, filter on ===
[13:07:35]   allow 046D:C216
[13:07:36] BLOCK GetPreparsedData  vid=1B1C pid=2B02
[13:07:36] allow GetAttributes  vid=046D pid=C216
```

**This log is also the proof that the shim loaded at all.** If the target starts and the file is never written, the loader did not pick up your copy of `hid.dll` and nothing here is taking effect. Check that first when it does not work.

Set `Path=` under `[Log]` to write somewhere else, worth doing if you shim more than one application.

---

## Building

```powershell
winget install --id MartinStorsjo.LLVM-MinGW.MSVCRT -e
```

Close the terminal and open a new one, so the compiler is on your `PATH`. Windows does not re-read environment changes into running processes.

```powershell
git clone https://github.com/mattruggio/hidshim.git
cd hidshim
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\scripts\Build-HidShim.ps1
```

Expect `Smoke test passed.`, `Build verified.` and `i386`. Then continue from step 2 above. A clone carries no download mark, so `Unblock-File` is not needed here.

The build is not just a compile. It reads the export table of the DLL it just produced and compares it against your real `C:\Windows\SysWOW64\hid.dll`, failing on any missing export. That matters more than it looks: DirectInput resolves every HID function by name with `GetProcAddress`, so a missing or decorated export does not fail loudly, it just makes DirectInput quietly believe the function is unavailable. It then runs a smoke test that loads the shim and enumerates devices the way DirectInput does, which is the only part that proves the naked forwarders actually execute.

LLVM-MinGW rather than Visual Studio because it ships the i686 target and its own `hidsdi.h`, and VS makes 32 bit support an optional component that is easy to miss. Use **Windows PowerShell 5.1** for the scripts.

---

## How it works

The interesting part is not the filter, it is where the filter sits.

DirectInput enumerates and parses **every** HID device on the machine internally, before it calls back into the application even once. Filtering above DirectInput therefore does nothing. The first attempt here was a `dinput.dll` proxy, and it failed even when tightened until the application opened no HID device at all. The heap was already destroyed by then.

So the filter has to sit below DirectInput:

```
SetupDiGetClassDevsW(HidGuid)      enumerate HID interfaces
CreateFile(devicePath)             open the device
HidD_GetAttributes                 read vendor and product ids
HidD_GetPreparsedData              fetch the report descriptor
HidP_GetCaps and friends           parse it   <- the damage happens here
```

`setupapi.dll` is earlier still, but it is listed in the `KnownDLLs` registry key, so the loader resolves it from the system directory no matter what sits beside the executable. `hid.dll` is **not** listed, and `dinput.dll` loads it by bare name with `LoadLibraryW`, so the application directory is searched first. That makes `hid.dll` the lowest point in this stack reachable from an application directory, and it sits in front of the parsing rather than behind it.

The shim forwards all 47 exports to the real `hid.dll` and filters exactly two. `HidD_GetAttributes` fails for any device not on the allow list, so DirectInput drops it before asking for anything else. `HidD_GetPreparsedData` fails for the same devices in case DirectInput reaches for the descriptor without checking attributes first, and since that is the call leading directly into the parser, it is the one that really has to hold.

Everything else is a naked tail jump, which is why the forwarders carry no signatures and cannot get one wrong.

---

## Limits

**32 bit only.** The forwarders jump through symbols named `_real_*`, and that leading underscore is i386 C symbol decoration, so the `THUNK` macro would not assemble for x86-64 as written. That is genuinely the only obstacle to a 64 bit build, but it has not been done, so it is not claimed.

**The target must load `hid.dll` by bare name.** True for `dinput.dll`, and for anything listing `hid.dll` in its import table. An application that loads it by an absolute `System32` path bypasses the shim entirely.

**Device enumeration only.** The `HidP_*` parsing functions are pass through. This hides devices, it does not sanitise malformed descriptors.

**It cannot hide a device the target genuinely needs.** If the culprit is also the device you want, this is the wrong tool.

**Tested against one application.** That it should generalise is an argument from mechanism, not a pile of test results. If you use it elsewhere, I would be glad to hear whether it held.

---

## Layout

```
scripts/Build-HidShim.ps1     compile, verify the export table, smoke test
scripts/Show-HidDevices.ps1   list HID devices as VID:PID
scripts/Use-HidShim.ps1       install, remove, status
src/hid-shim.c                the shim
src/hid-shim.ini              configuration, copied in alongside the DLL
src/test-hid-shim.c           the smoke test
```

A release zip has the same layout, minus the sources, so the scripts work unchanged from an extracted copy.

## Releasing

Run the **build** workflow from the Actions tab with a `version` such as `1.0.0`. It validates the version, builds, and only then creates the tag and the release together, so a failed build cannot tag something that does not compile and a failed publish cannot leave a tag pointing at nothing. It refuses a version that is already tagged.

The release job publishes the artifact the build job verified rather than compiling again, so the DLL you download is the one that passed the export check. Only that job holds a token that can write; the build that runs on every pull request is read only. Attestation requires the repository to be public, since GitHub rejects it for user-owned private repositories.

| Bump | When |
|---|---|
| Major | An ini key or a script parameter is renamed or removed |
| Minor | A new capability that existing configs ignore, such as `[Log] Path` |
| Patch | A fix that needs no config change |

The 47 exports are not ours to version, since Windows sets them. If a future Windows adds an export to the real `hid.dll`, the build starts failing on machines that have it, and fixing that is a patch release whether we like it or not.

## Licence

MIT. See [LICENSE](LICENSE).
