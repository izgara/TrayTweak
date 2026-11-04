# TrayTweak

A very small tool that tidies the Windows 11 taskbar tray. It can hide the icons Windows
gives you no switch for, and lets you pick which app icons stay on the taskbar.

Windhawk and ExplorerPatcher can do this too and do much more. TrayTweak only does this one
job, and nothing of it keeps running in the background except what is described below.

**Türkçe özet:** Windows 11 görev çubuğundaki ses, mikrofon ve dil simgelerini gizler, hangi
uygulama simgelerinin görüneceğini küçük bir panelden seçmeni sağlar. Her şey varsayılan olarak
kapalıdır. Panelde "Türkçe arayüz" seçeneği vardır.

## Features

All options are off until you turn them on, and all of them apply right away.

- Hide the volume icon
- Hide the microphone privacy icon
- Hide the language indicator (ENG TRQ). Only the indicator is hidden: your keyboard layouts
  and shortcuts for switching them are not changed.
- Hide the overflow arrow (^), so a hidden icon is really gone instead of moved to a menu
- Always show icons, including those of apps you install later
- Choose which app icons stay on the taskbar, including Safely Remove Hardware
- Start with Windows

The icons are recognised by symbol rather than by name, so this works in every display language.

## Install

1. Download the zip from [Releases](../../releases) and unpack it to a folder you will keep.
2. Run `TrayTweakPanel.exe` and tick what you want.
3. Tick **Start with Windows** to have it applied at every sign-in.

The first time you hide a Windows icon, Windows asks for admin rights once (see below). The
files aren't code-signed, so SmartScreen may warn you. You can check the download against the
SHA-256 in the release notes, or build it yourself.

Tested on Windows 11 build 26200, x64.

## What it does to your PC

- `TrayTweakPanel.exe` only runs while its window is open.
- `TrayTweak.exe` runs once at sign-in, applies your settings and exits.
- `TrayTweakTap.dll` is loaded into `explorer.exe` if you turned on an option that needs it.
  This is the only way to reach these icons. It finds them through XAML diagnostics, disconnects,
  and from then on holds only a few XAML event handlers and two registry waits. It has no
  dedicated thread or timer of its own and it doesn't load into any other process.
- If Explorer restarts, the icons come back until `TrayTweak.exe` runs again.
- It uses no network.

Settings are stored in `HKCU\Software\TrayTweak`. It also changes these values:

| Value | Why |
| --- | --- |
| `IsPromoted` under `HKCU\Control Panel\NotifyIconSettings\*` | The Windows setting for showing an app's icon on the taskbar |
| `SystemTrayChevronVisibility` under `HKCU\Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\TrayNotify` | The Windows setting for the overflow arrow |
| `HKLM\Software\Microsoft\XAML\Debug\DisableCompositionDiag` = 1 | Avoids a known heap corruption risk in XAML diagnostics. This is why admin rights are needed once. |

## Good to know

- Hiding the microphone icon removes the sign that an app is using your microphone. You can
  still check this under Settings > Privacy & security > Microphone.
- With the arrow hidden, a hidden app's tray menu can't be reached. Tick the app in the panel
  to bring its icon back.
- A Windows update can change the taskbar and stop this from working. At worst, I would expect
  the icons to come back or Explorer to restart, but that can't be guaranteed.
- Security software may object to a DLL being loaded into Explorer.
- It doesn't touch game processes, but it hasn't been tested with anti-cheat systems such as
  Vanguard.
- If something goes wrong, `TrayTweak.log` next to the program says what happened. It only
  lists names of tray entries and icon symbols.

## Uninstall

Untick everything in the panel, including **Start with Windows**. Then remove what is left,
restart Explorer and delete the folder:

```
reg delete "HKCU\Software\TrayTweak" /f
reg delete "HKLM\Software\Microsoft\XAML\Debug" /v DisableCompositionDiag /f
```

The second command needs an administrator prompt.

## Build

You need the Visual Studio 2022 Build Tools (C++) and the Windows SDK. Run `build.cmd dll`.
`build.cmd` on its own skips the DLL, which is locked while Explorer has it loaded: to replace
it, rename the old one first and restart Explorer afterwards.

## Credits and license

GPL-3.0, see [LICENSE](LICENSE). Recognising the icons and handling their layout is based on
[Taskbar tray system icon tweaks](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-tray-system-icon-tweaks.wh.cpp)
by m417z. The way of connecting to XAML diagnostics is based on
[Windows 11 Taskbar Styler](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/windows-11-taskbar-styler.wh.cpp).
Both are GPL-3.0. TrayTweak isn't affiliated with Windhawk or Microsoft.
