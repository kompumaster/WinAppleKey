# WinAppleKey

**English** · [Русский](README_RU.md)

Apple Magic Keyboard (A1644) and Apple Wireless Keyboard (A1314) driver for Windows.

![Apple Wireless Keyboard (A1314)](A1314.jpg)

This is a fork of [samartzidis/WinAppleKey](https://github.com/samartzidis/WinAppleKey), the original A1644 driver for Windows 10, which is archived now. A1314 support comes from [jeba1215's fork](https://github.com/jeba1215/WinAppleKey); on top of that this fork adds real media keys, volume, brightness, sleep/hibernate, and the MSI installer.

Windows will type on an A1314 out of the box, so the keyboard isn't broken. What you don't get is the bottom-left corner you're used to. The leftmost key acts as [Fn] instead of [Ctrl], [Delete] is hidden behind [Fn], [⏏ eject] does nothing, and the media, volume and brightness keys are dead. This driver fixes that.

Tested on an A1314 (ISO, PID `0x0256`) over Bluetooth on Windows 11 24H2, build 26100, on a Lenovo ThinkBook - that laptop is what the [Fn] layer, the brightness keys and the [Alt]+[Tab] behaviour were worked out against. The A1644 side comes from the upstream project, but I don't own that keyboard, so it hasn't been re-tested since I changed things. Assume the A1314 path is the well-trodden one.

## Keys

Hold the physical [Ctrl] key for the [Fn] combinations. Yes, [Ctrl] - that's what the [Fn]/[Ctrl] swap is for.

| You press | You get |
| --- | --- |
| [Fn]+[F1], [Fn]+[F2] | brightness down, brightness up |
| [Fn]+[F3] | [Win]+[Tab], Task View |
| [Fn]+[F4] | [Win]+[Z], Snap Layouts |
| [Fn]+[F5] | sleep |
| [Fn]+[F6] | hibernate |
| [Fn]+[F7], [F8], [F9] | previous track, play/pause, next track |
| [Fn]+[F10], [F11], [F12] | mute, volume down, volume up |
| [Fn]+[←] | [Home] |
| [Fn]+[→] | [End] |
| [Fn]+[↑] | [Page Up] |
| [Fn]+[↓] | [Page Down] |
| [Fn]+[Return] | [Insert] |
| [Fn]+[P], [Fn]+[S], [Fn]+[B] | [Print Screen], [Scroll Lock], [Pause/Break] |
| [Fn]+[Ctrl] | [Right Ctrl] |

[⏏ eject] is [Delete].

Any other [Fn]+key combination is deliberately swallowed. [Fn] never reaches Windows as [Ctrl], so you can't trigger a shortcut by accident, and a half-typed [Fn] chord doesn't start doing strange things in whatever window happens to be focused.

On the left, [fn] and [Ctrl] are swapped, [⌥ alt] becomes [Win] and [⌘ cmd] becomes [Alt]. The right side only has two keys to work with, and one of them is the context-menu key. By default the right [⌘ cmd] is [Alt] and the right [⌥ alt] opens the menu. Set `SwapRightCmdAlt` to 0 if you'd rather have the menu on [⌘ cmd].

That [Alt] is the **left** one on purpose. Windows treats [Right Alt] as AltGr, and the task switcher never gets the "commit" from a VK_RMENU release - [Alt]+[Tab] with it leaves the window list on screen waiting for [Enter].

One thing that trips people up: [Alt], [Ctrl] and [Win] do nothing on their own. If you press the right [⌥ alt] and nothing happens, that's [Alt] behaving normally. Try [⌥ alt]+[Tab].

## How it works

The driver is a lower filter on the HID device node, so it sits between the transport (USB or Bluetooth) and HIDCLASS and gets to rewrite input reports before anything parses them. No user-mode software is involved.

The two keyboards don't look alike on the wire. The A1644 sends 11-byte reports. The A1314 sends 10-byte keyboard reports plus 3-byte "special key" reports that carry [⏏ eject] and [Fn] in the low bits. `A1314.c` and `A1644.c` each know their layout and translate it, then both go through the same rules.

Volume and brightness needed a different trick. The A1314's report descriptor has no Mute, Volume or Brightness usages at all, and Windows ignores the ones that exist on the keyboard page, so there was nothing to rewrite in the report. Instead the driver also installs itself as a tiny root-enumerated device and creates a virtual Consumer Control device through the Virtual HID Framework. Windows sees a second, ordinary multimedia keyboard with Mute, Vol+/Vol- and Brightness+/Brightness-, and [Fn]+[F1]/[F2] plus [Fn]+[F10]..[F12] submit reports to it. That's also why you'll find a "WinAppleKey Virtual HID Device" in Device Manager.

Brightness is the part that surprised me. External keyboards with brightness keys normally do nothing on a Windows laptop, so I expected the Consumer usages to be ignored too. They aren't: 0x6F/0x70 go straight to the internal panel, and the log shows the brightness changing about 7 ms after the report. It won't touch an external monitor, though - that would be DDC/CI, which is a separate problem.

Sleep and hibernate aren't HID at all, so the driver talks to the power manager directly. The obvious call, `NtSetSystemPowerState`, turns out not to be exported by ntoskrnl; the exported one is `ZwInitiatePowerAction`. It needs PASSIVE_LEVEL, while reports are parsed in a completion routine that can run at DISPATCH_LEVEL, so the call is handed to a worker thread. The keys only fire on the press, never while held. A level-triggered version would put the machine straight back to sleep the moment it woke up.

## Installing

There is an MSI that does all of the below, including the certificate and the virtual device - see "Building the installer". The manual route is here for when you want to see what it is doing, or do it by hand.

The driver is signed with a self-signed test certificate, so Windows will only load it when **TESTSIGNING** is on. If Secure Boot is enabled you have to turn it off in the firmware first. On Windows 10 1607 and later, including 11, you also have to turn off Memory Integrity (Core isolation), otherwise test-signed drivers are rejected no matter what.

```
bcdedit /set TESTSIGNING ON
```

Then reboot. Running Windows in TESTSIGNING permanently does weaken it, so decide whether that's a trade you want before you start. This is free software that you install at your own risk.

Build the driver, then trust the certificate the build produced. It's exported next to the driver as `WinAppleKey\x64\<Configuration>\WinAppleKey.cer` and is generated automatically on the first build:

```
certutil -addstore -f Root             WinAppleKey\x64\Release\WinAppleKey.cer
certutil -addstore -f TrustedPublisher WinAppleKey\x64\Release\WinAppleKey.cer
pnputil /add-driver WinAppleKey\x64\Release\WinAppleKey\WinAppleKey.inf /install
```

The `.cer`, `.pfx` and `.pvk` files sitting next to the sources are the original author's and aren't used any more.

That last command installs the keyboard filter. The volume keys need one more step, because they live on the separate virtual device:

```
devcon install WinAppleKey\x64\Release\WinAppleKey\WinAppleKey.inf root\WinAppleKeyVhf
```

`devcon.exe` ships with the WDK. Skip this and everything still works except [Fn]+[F1]/[F2] and [Fn]+[F10]..[F12], which will just do nothing.

**Reinstalling.** `pnputil /delete-driver ... /uninstall` does not always remove the root node of the virtual device. If it survives, `pnputil /add-driver /install` reuses it and then `devcon install` creates a second one. The driver keeps one VHF handle for the whole driver, so the second host deletes the first host's device - and then removing either host leaves you with no virtual device at all. The symptom is nasty: the keyboard still works, everything that doesn't need the virtual device keeps working, and only volume and brightness are silently dead.

So before `add-driver` on a reinstall, make sure nothing is left:

```
devcon remove root\WinAppleKeyVhf
```

`devcon remove` takes a hardware ID and removes every matching node, which is what you want here. Afterwards there should be exactly one device with hardware ID `root\WinAppleKeyVhf`. If you do end up with two, remove both and run the `devcon install` line once. If there is one but volume and brightness are dead, restart it - that re-runs `VhfInit` and recreates the virtual device:

```
pnputil /restart-device "ROOT\HIDCLASS\0000"
```

To remove it all, drop the virtual device first, then the package, then turn test signing off again:

```
devcon remove root\WinAppleKeyVhf
pnputil /delete-driver oemXX.inf /uninstall /force
bcdedit /set TESTSIGNING OFF
```

## Settings

Four DWORDs under `HKLM\SYSTEM\CurrentControlSet\Services\WinAppleKey`.

| Value | Default | What it does |
| --- | --- | --- |
| `SwapFnCtrl` | 1 | [fn] and [Ctrl] swap places |
| `SwapAltCmd` | 1 | [⌥ alt] and [⌘ cmd] swap places, left side only |
| `RightCmdAsMenu` | 1 | one of the right-hand modifiers becomes the context-menu key |
| `SwapRightCmdAlt` | 1 | 0: right [⌘ cmd] is the menu key, right [⌥ alt] is [Alt]. 1: the other way round |

These live under the driver's service key, so a full uninstall takes them with it and the next install starts from the INF defaults again. Worth remembering if you change something and then reinstall the package.

They're read once, when the driver loads, so a reboot or a reinstall is needed after a change. Reinstalling is faster than rebooting:

```
pnputil /delete-driver oemXX.inf /uninstall /force
devcon remove root\WinAppleKeyVhf
pnputil /add-driver WinAppleKey\x64\Release\WinAppleKey\WinAppleKey.inf /install
devcon install WinAppleKey\x64\Release\WinAppleKey\WinAppleKey.inf root\WinAppleKeyVhf
```

That last line is not optional - the reinstall takes the virtual device with it, and without it volume and brightness are gone. See the note above about leftovers if they don't come back.

## Building

Visual Studio 2019 or later, plus the WDK. The project builds with both the Windows 10 WDK (10.0.19041.0) and the 24H2 one (10.0.26100.0), but `vhfkm.lib` has to come from 19041 - the 26100 install I used only shipped part of it. It's wired up through `AdditionalDependencies` in the project file.

```
msbuild WinAppleKey\WinAppleKey.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
```

The build signs the `.sys` and the `.cat` with a test certificate, so there is no separate signing step. The certificate is looked up by subject, so create it once before the first build:

```
New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=WinAppleKey Test Certificate' -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(10)
```

Letting the WDK generate its own would name it after your account and machine (`WDKTestCert <user>,<ticks>`), and that name ends up inside the `.sys`, the `.cat` and the MSI - not something to hand out in a release.

Versions have four fields and start at zero: `0.1.1.11`. The MSI gets the three trailing ones, `1.1.11`, because MSI only ever compares three; the leading zero just says this is not a commercially released product. Bump `DriverVer` in `WinAppleKey.inf`, the numbers in `resource.rc` and `Version` in `Setup.wixproj` together.

The code is split by job: `Driver.c` is the IRP plumbing, `A1314.c` and `A1644.c` translate the two report formats, `Vhf.c` is the virtual device, `Power.c` is sleep and hibernate.

## Building the installer

There is a WiX MSI project under `Setup`. Building the solution does the whole thing in order, driver first, then `DeviceTool.exe`, then the package:

```
msbuild WinAppleKey.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64
```

The result is `Setup\x64\Release\WinAppleKey-Setup.msi`. It trusts the certificate in both `Root` and `TrustedPublisher`, copies the driver to `%ProgramFiles%\WinAppleKey`, installs the package and creates the root-enumerated device that hosts the virtual HID device. Uninstalling takes all of it back out, certificate included. It refuses to install when TESTSIGNING is off, because the driver would not load anyway.

Uninstall is the part that needed care. A filter driver that is still bound to a keyboard cannot be stopped - the call simply does not return, and the installer hangs on it - so the package deliberately has no `ServiceControl` with `Stop`. What happens instead: the device node goes first, DIFxApp takes the package out (which unbinds the filter), and only then does `DeviceTool cleanup` delete the service and the copy of the `.sys` that the INF leaves in `System32\drivers`. A whole uninstall takes about two seconds and leaves nothing behind.

WiX v3 is not installed system-wide: `WixBinDir` has to point at its `bin` directory. Set it once as an environment variable (`setx WixBinDir C:\path\to\wix`) or pass `/p:WixBinDir=...` per build. Nothing in the project hardcodes a path.

`Setup\DeviceTool` is a small console program that creates and removes that root device. Windows has no built-in equivalent: `pnputil` installs a driver onto a device that already exists, but it cannot create one, and shipping `devcon.exe` from the WDK would mean redistributing a Microsoft tool. It gets installed to `%ProgramFiles%\WinAppleKey` as well, which is handy when the virtual device gets stuck:

```
DeviceTool.exe remove root\WinAppleKeyVhf
DeviceTool.exe create root\WinAppleKeyVhf "C:\Program Files\WinAppleKey\WinAppleKey.inf"
DeviceTool.exe cleanup WinAppleKey WinAppleKey.sys
```

`cleanup` is the uninstall helper: it stops the service (waiting at most three seconds), deletes it, and removes the `.sys` from `System32\drivers`, retrying for a moment because PnP releases the file asynchronously. If it is still mapped after that it leaves it alone on purpose - during an upgrade the old product is removed while the driver is loaded, and a delete-on-reboot order would then wipe the copy the new version has just installed. A leftover file is harmless: with the service and the package gone, nothing loads it, and the next install overwrites it.

## If you're going to touch the virtual HID device

Read this before you do, because the failure mode is a bugcheck, not an error message.

**The FDO has to be attached.** `VhfCreate` finds `vhf.sys` by calling `IoGetLowerDeviceObject` on the device object you hand it. A root device created with a bare `IoCreateDevice` has nothing below it, so you get `STATUS_INVALID_PARAMETER` and no hint about why. Call `IoAttachDeviceToDeviceStack` first. KMDF does that for you inside `WdfDeviceCreate`, which is why the samples just work.

**`reportBuffer` starts with the report ID.** `HID_XFER_PACKET` wants `{reportId, data...}` and `reportBufferLen` has to include that byte. The WDK miniport samples give it away (`packet.reportBuffer + sizeof(packet.reportId)`). Hand it a data-only buffer and HIDCLASS reads your first data byte as a report ID and drops the whole thing, silently.

**[Delete] the virtual device before the remove request goes down.** `vhf.sys` is a KMDF driver; if you call `VhfDelete` after forwarding `IRP_MN_REMOVE_DEVICE` you're talking to a filter that is already being torn down, and you get `0x50 PAGE_FAULT_IN_NONPAGED_AREA` in `wdf01000.sys`. Do it first, and keep report submission out of the way while it happens (a spinlock and a flag will do).

**Report bits are assigned in declaration order.** The usages in the report descriptor become bit 0, bit 1, bit 2 and so on, so inserting a usage in the middle silently shifts everything after it. If you add one, keep the `VhfBit*` defines in `Driver.h` in step with the descriptor in `Vhf.c`. Brightness (0x6F/0x70) is worth knowing about too: it looks like the sort of usage Windows would ignore, but it works.

**There is one VHF handle for the whole driver, not one per device.** That is fine as long as exactly one `root\WinAppleKeyVhf` node exists, but it makes duplicates actively harmful: the second host's `VhfInit` deletes the first host's virtual device, and the surviving host will not create another one until it is restarted. If volume and brightness stop working while the rest of the driver is fine, look for two root nodes before you look at the code.

There's one more thing in the INF: the root device needs `LowerFilters = vhf`, and its `AddService` needs the `SPSVCINST_ASSOCSERVICE` flag, otherwise PnP refuses to install it as a function driver.

All of this is much easier to get wrong than to debug on the machine you're using. Do it in a VM with Driver Verifier pointed at the driver.

## Questions

**Can this be properly signed so I don't need TESTSIGNING?**
No. Kernel-mode drivers need an EV certificate plus Microsoft's attestation, and that means a registered company and real money. This is a hobby project, so it stays test-signed.

**TESTSIGNING won't turn on: "The value is protected by Secure Boot policy".**
Secure Boot is on. Turn it off in the firmware.

**The multimedia keys don't work.**
They do now, natively. [Fn]+[F7]..[F9] emit real Consumer usages, so Windows handles them itself. `MapMultimediaKeys.ahk` is only kept for history - it maps the old [F19]..[F24] scheme and isn't needed any more.

**Brightness doesn't do anything.**
[Fn]+[F1]/[F2] go through the virtual device, so without it they're silent no-ops - check that "WinAppleKey Virtual HID Device" is present. And there has to be an internal panel to dim: the usages reach the laptop display, not an external monitor.

**[Ctrl]+[Shift]+[T] doesn't reopen a closed tab in Chrome.**
That's the keyboard, not the driver: in [fn] + [Left Shift] + [T] the A1314 never sends the T at all. The same modifiers with another key work: [Ctrl]+[Shift]+[Esc] opens Task Manager. So it's the T key that goes missing, not the modifiers. Use [Ctrl]+[Right Shift]+[T].

**[F5] does nothing, or the machine wakes up again right away.**
The platform may not support the sleep state being requested; Modern Standby machines have no S3 and use S0 low-power idle instead. For hibernate, check that hibernation is enabled at all (`powercfg /h on`) - otherwise [F6] is a silent no-op.

**Why is there a second "WinAppleKey Virtual HID Device"?**
That's the virtual Consumer Control device that provides brightness and the volume keys. Remove that device and you lose those, nothing else.

**The right [⌥ alt] / [⌘ cmd] does nothing.**
It's a modifier. Modifiers don't produce events on their own. Try it with [Tab].

**Which keyboards are supported?**
The A1644 and the A1314 (all three layouts: `0x0255` ANSI, `0x0256` ISO, `0x0257` JIS). The INF also registers the A1255 (`0x0239`-`0x023B`) and the older Apple Wireless Keyboard (`0x022C`-`0x022E`, `0x0208`-`0x020A`), following Apple's own `KeyMagic64.inf` - those use the same report format and should behave the same, but I haven't tried them.

Note that installing this replaces the `LowerFilters` entry that Apple's BootCamp `KeyMagic` driver uses. The two aren't meant to run together.

**Can I give you a stack trace from `0x50`?**
Not to me personally, but yes, it's useful: driver verifier plus a dump beats guessing every time. That's how the teardown ordering above was found.
