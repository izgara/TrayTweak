# TrayTweak

A very small tool that tidies the Windows 11 taskbar. It can hide the tray icons Windows
gives you no switch for, lets you pick which app icons stay on the taskbar, puts a light
volume mixer in the tray, and can make the taskbar see-through.

Windhawk and ExplorerPatcher can do this too and do much more. TrayTweak only does these few
jobs, and nothing of it keeps running in the background except what is described below.

**Türkçe özet:** Windows 11 görev çubuğundaki ses, mikrofon ve dil simgelerini gizler, hangi
uygulama simgelerinin görüneceğini küçük bir panelden seçmeni sağlar. Tepside uygulama bazlı
ses karıştırıcısı ve buzlu cam ya da tamamen şeffaf görev çubuğu seçeneği de vardır. Her şey
varsayılan olarak kapalıdır. Panelde "Türkçe arayüz" seçeneği vardır.

## Features

All options are off until you turn them on, and all of them apply right away.

- Hide the volume icon
- Volume mixer in the tray, in place of the volume icon: a slider for the output device and one
  per app, with level meters. Click an icon to mute, scroll over a row to change it, click the
  device name or right-click the tray icon to switch the output device, middle-click the tray
  icon to mute. Apps that play through a helper process (such as Spotifast) get one slider.
- Hide the microphone privacy icon
- Hide the language indicator (ENG TRQ). Only the indicator is hidden: your keyboard layouts
  and shortcuts for switching them are not changed.
- Frosted glass taskbar, or a fully clear one (see [Glass](#glass))
- Hide the overflow arrow (^), so a hidden icon is really gone instead of moved to a menu
- Always show icons, including those of apps you install later
- Choose which app icons stay on the taskbar, including Safely Remove Hardware
- Start with Windows

The icons are recognised by symbol rather than by name, so this works in every display language.

## Install

1. Download `TrayTweakSetup.exe` from [Releases](../../releases) and run it.
2. Tick what you want in the panel that opens. It is also in the Start menu as **TrayTweak**.
3. Tick **Start with Windows** to have it applied at every sign-in.

Setup installs for your account only, in `%LOCALAPPDATA%\Programs\TrayTweak`, and needs no
admin rights. Running a newer setup updates it and keeps your settings. The first time you
hide a Windows icon, Windows asks for admin rights once (see below).

The files aren't code-signed, so SmartScreen may warn you. You can check the download against
the SHA-256 in the release notes, or build it yourself. The zip in the release has the same
files without the setup, if you'd rather keep them in a folder of your own.

Tested on Windows 11 build 26200, x64.

## Glass

**Frosted glass taskbar** makes the taskbar's own background transparent and gives it the
Windows 11 acrylic blur, tinted with `GlassColor` at `GlassOpacity` percent. With
`GlassOpacity` at 0 the taskbar is fully clear instead: no tint, no blur, no top edge line.

There is no colour picker yet. Set the values in `HKCU\Software\TrayTweak`; they apply at once:

| Value | Type | Default | |
| --- | --- | --- | --- |
| `GlassColor` | DWORD | `0x304E73` | Tint as 0xRRGGBB |
| `GlassOpacity` | DWORD | `50` | Tint strength, 0 to 100. 0 means fully clear. |

Windows 11 no longer has the plain Windows 7 style blur: asking for it gives a black taskbar,
which is why acrylic is used. Transparency effects must be on in Settings > Personalisation >
Colours. [DWMBlurGlass](https://github.com/Maplespe/DWMBlurGlass) with "Override AccentBlur
effect" turns the acrylic into a Windows 7 Aero blur, if you want that and accept that it runs
inside `dwm.exe`.

## What it does to your PC

- `TrayTweakPanel.exe` only runs while its window is open.
- `TrayTweak.exe` runs once at sign-in, applies your settings, starts the mixer if it is on,
  and exits. Running it again puts everything back if something reset the taskbar.
- `TrayTweakVolume.exe` is the mixer. It runs while the option is on and uses about 2 to 5 MB.
  Its meters and session list only update while the mixer is open.
- `TrayTweakTap.dll` is loaded into `explorer.exe` if you turned on an option that needs it.
  This is the only way to reach these icons. It finds them through XAML diagnostics, disconnects,
  and from then on holds only a few XAML event handlers, one thread that sleeps until a setting
  or tray entry changes, and for the glass option a window subclass and an event hook. It has
  no timer and doesn't load into any other process.
- If Explorer restarts, everything comes back until `TrayTweak.exe` runs again.
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
- Switching the output device uses an undocumented but long-standing Windows interface, the
  same one other device switchers use.
- A Windows update can change the taskbar and stop this from working. At worst, I would expect
  the icons to come back or Explorer to restart, but that can't be guaranteed.
- Security software may object to a DLL being loaded into Explorer.
- It doesn't touch game processes, but it hasn't been tested with anti-cheat systems such as
  Vanguard.
- If something goes wrong, `TrayTweak.log` next to the program says what happened. It only
  lists names of tray entries and icon symbols.

## Uninstall

Remove **TrayTweak** in Settings > Apps > Installed apps. It closes the mixer, removes the
settings, the autostart entry and the Start menu shortcut, shows the overflow arrow again,
restarts Explorer once and deletes the folder.

The one machine-wide value can then be removed from an administrator prompt:

```
reg delete "HKLM\Software\Microsoft\XAML\Debug" /v DisableCompositionDiag /f
```

If you used the zip, untick everything in the panel first, then delete the folder and run
`reg delete "HKCU\Software\TrayTweak" /f`.

## Build

You need the Visual Studio 2022 Build Tools (C++) and the Windows SDK. Run `build.cmd dll`.
`build.cmd` on its own skips the DLL, which is locked while Explorer has it loaded: to replace
it, rename the old one first and restart Explorer afterwards. Both build `TrayTweakSetup.exe`
last, with the other files embedded.

## Credits and license

GPL-3.0, see [LICENSE](LICENSE). Recognising the icons and handling their layout is based on
[Taskbar tray system icon tweaks](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-tray-system-icon-tweaks.wh.cpp)
by m417z. The way of connecting to XAML diagnostics is based on
[Windows 11 Taskbar Styler](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/windows-11-taskbar-styler.wh.cpp).
Both are GPL-3.0. TrayTweak isn't affiliated with Windhawk or Microsoft.
