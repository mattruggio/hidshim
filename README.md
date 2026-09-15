# hidshim

Hide HID devices from a single Windows process, without touching the rest of the machine.

This is a proxy `hid.dll`. You drop it next to an executable, name the devices that executable is allowed to see, and every other HID device becomes invisible to it. Nothing is installed, no driver, no service, nothing in the background. Uninstalling is deleting two files.

It exists because this is what it took to get **NBA Live 2005**, a PC game from 2004, running in 2026 on a Core i5-13600K with an RTX 2060. The game died a few seconds after launch with a corrupted heap, every time. The cause was not the GPU, the OS version, or the age of the game. It was the keyboard: a modern Corsair keyboard presents fifteen HID collections, and DirectInput's report descriptor parser cannot cope with that many. Hiding everything except the gamepad fixed it outright.

That failure mode is not specific to one game. Any DirectInput era application can hit it, and modern peripherals make it more likely every year: RGB software, virtual HID buses, wireless receivers, and composite devices all inflate the enumeration that has to be parsed.

**Scope: this is a 32 bit DLL for 32 bit processes.** See [Limits](#limits).

---

## How it works

The interesting part is not the filter, it is *where* the filter sits.

DirectInput enumerates and parses **every** HID device on the machine internally, before it calls back into the application even once. So filtering above DirectInput does not work. The first attempt here was a `dinput.dll` proxy that filtered the device list handed to the application, and it failed even when tightened until the application opened no HID device at all. The heap was already destroyed by then.

The filtering has to happen *below* DirectInput. The call sequence is:

```
SetupDiGetClassDevsW(HidGuid)      enumerate HID interfaces
SetupDiEnumDeviceInterfaces
SetupDiGetDeviceInterfaceDetailW
CreateFile(devicePath)             open the device
HidD_GetAttributes                 read vendor and product ids
HidD_GetPreparsedData              fetch the report descriptor
HidP_GetCaps and friends           parse it   <- the damage happens here
```

`setupapi.dll` is the earlier point, but it cannot be intercepted: it is listed in the `KnownDLLs` registry key, so the loader resolves it from the system directory no matter what sits beside the executable. `hid.dll` is **not** listed, and `dinput.dll` loads it by bare name with `LoadLibraryW`, so the standard search order applies and the application directory is searched first.

That makes `hid.dll` the lowest point in this stack reachable from an application directory, and it sits in front of the parsing rather than behind it.

The shim forwards all 47 exports to the real `hid.dll` in the system directory and filters exactly two:

* `HidD_GetAttributes` fails for any device not on the allow list, so DirectInput drops it before asking for anything else.
* `HidD_GetPreparsedData` fails for the same devices, in case DirectInput reaches for the descriptor without checking attributes first. This is the call that leads directly to the parsing, so it is the one that really has to hold.

Everything else is a naked tail jump, which is why the forwarders carry no signatures and cannot get one wrong.

### Why an allow list, and only an allow list

There is deliberately no block list. Two reasons, and the first is fatal on its own:

1. A rule can only name a vendor id, and a **virtual HID bus**, such as the one iCUE installs, does not have one. No block list can express it.
2. An allow list asks the easier question. You have to know which device you *want*, normally just your gamepad. A block list demands you first work out which device is at *fault*, which is the hard part.

An empty allow list is therefore the safe default rather than an unconfigured one: everything is hidden, which cannot crash.

Your keyboard and mouse still work. DirectInput supplies those from its system keyboard and mouse objects, which never pass through this enumeration.

---

## What you need

* Windows, 64 bit
* **LLVM-MinGW**, which ships the i686 target and its own `hidsdi.h`

```powershell
winget install --id MartinStorsjo.LLVM-MinGW.MSVCRT -e
```

Then close the terminal and open a new one, so the compiler is on your `PATH`. Windows does not re-read environment changes into running processes.

Visual Studio can build this too, but its 32 bit support is an optional component that is easy to miss, so this repo standardises on LLVM-MinGW.

Use **Windows PowerShell 5.1** for the scripts. PowerShell 7 will mostly work, but `Show-HidDevices.ps1` leans on WMI in ways that behave differently.

---

## 1. Build

```powershell
git clone https://github.com/mattruggio/hidshim.git
cd hidshim
.\scripts\Build-HidShim.ps1
```

**Check:** the output ends with `Smoke test passed.` and `Build verified.`, and reports `i386`.

The build is not just a compile. It reads the export table of the DLL it just produced and compares it against your real `C:\Windows\SysWOW64\hid.dll`, failing on any missing export. That check matters more than it looks: DirectInput resolves every HID function by name with `GetProcAddress`, so a missing or decorated export does not fail loudly, it just makes DirectInput quietly believe the function is unavailable.

It then compiles and runs a smoke test that loads the shim and enumerates HID devices the way DirectInput does. The export table proves the names exist; only the smoke test proves the naked forwarders actually execute and that the ini is being read.

---

## 2. Configure

Find your device ids:

```powershell
.\scripts\Show-HidDevices.ps1
```

This prints every HID device as `VID:PID`, grouped by vendor, which is the format the config wants. It handles Bluetooth LE devices too, which spell their ids differently (`VID&02046D` rather than `VID_046D`) and are otherwise easy to miss entirely.

A vendor with a lot of entries is the usual suspect: each collection is more work for the parser.

Then edit `src\hid-shim.ini` and name what you want kept:

```ini
[Filter]
Enabled=1
AllowDevices=046D:C216
```

That is a Logitech F310 gamepad. Comma separate for more than one. A bare vendor id such as `046D` covers every product from that vendor, but prefer the full `VID:PID`, since the company that made your gamepad may well have made a keyboard you want hidden.

**If you do not know what to allow yet,** set `Enabled=0` and install. The shim then forwards everything untouched and becomes a pure logger, so you can see exactly which devices the target opens and in what order.

---

## 3. Install

Point it at the directory holding the executable you want to shim:

```powershell
.\scripts\Use-HidShim.ps1 -Install -TargetDir 'C:\Program Files (x86)\EA SPORTS\NBA LIVE 2005'
```

Accept the UAC prompt. The script self-elevates because the target is usually under `Program Files`, and Windows has no in place privilege escalation, so it must relaunch itself rather than elevate the shell you are in.

It copies two files, `hid.dll` and `hid-shim.ini`, and nothing else. It will refuse to overwrite a `hid.dll` it did not build, on the grounds that a real `hid.dll` has no business being in an application directory and an unrecognised one is more likely a mistyped path than something to clobber.

To see what is currently installed, or to take it back out:

```powershell
.\scripts\Use-HidShim.ps1 -Status  -TargetDir '...'
.\scripts\Use-HidShim.ps1 -Remove  -TargetDir '...'
```

---

## 4. Verify

Start the target, then:

```powershell
Get-Content "$env:LOCALAPPDATA\hidshim\hid-shim.log" -Tail 20
```

You should see the devices you allowed and the ones that were blocked:

```
[13:07:35] === hid shim loaded, filter on ===
[13:07:35]   every device not listed below is hidden from the target
[13:07:35]   allow 046D:C216
[13:07:36] BLOCK GetPreparsedData  vid=1B1C pid=2B02
[13:07:36] allow GetAttributes  vid=046D pid=C216
```

**This log is also the proof that the shim loaded at all.** If the target starts and the file is never written, the loader did not pick up your copy of `hid.dll`, and nothing here is taking effect. That is the first thing to check when it does not work.

Set `Path=` under `[Log]` to write somewhere else, which is worth doing if you shim more than one application on the same machine.

---

## Limits

**32 bit only.** The forwarders jump through symbols named `_real_*`, and that leading underscore is i386 C symbol decoration. The `THUNK` macro would not assemble for x86-64 as written. That is genuinely the only obstacle to a 64 bit build, but it has not been done, so it is not claimed.

**The target must load `hid.dll` by bare name.** True for `dinput.dll`, which is the case this was built for, and true for anything that lists `hid.dll` in its import table. An application that loads it by an absolute `System32` path bypasses the shim entirely.

**Device enumeration only.** The `HidP_*` parsing functions are pass through. This hides devices, it does not sanitise malformed descriptors.

**It cannot hide a device the target genuinely needs.** If the culprit is also the device you want, this is the wrong tool.

**Tested against one application.** It was developed and measured against NBA Live 2005. That it should generalise is an argument from mechanism, not a pile of test results. If you use it somewhere else, I would be glad to hear whether it held.

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

---

## Licence

MIT. See [LICENSE](LICENSE).
