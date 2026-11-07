@echo off
rem build.cmd      builds TrayTweak.exe, TrayTweakPanel.exe, TrayTweakVolume.exe and TrayTweakSetup.exe
rem build.cmd dll  also builds TrayTweakTap.dll, which is locked while Explorer has it
rem                loaded: rename the old DLL first, or restart Explorer
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set FLAGS=/nologo /std:c++20 /utf-8 /EHsc /O1 /MT /W3 /permissive- /bigobj /DUNICODE /D_UNICODE /DNDEBUG /Gy /Gw
if /i "%~1"=="dll" (
    cl %FLAGS% /LD TrayTweakTap.cpp /link /OPT:REF /OPT:ICF /DEF:TrayTweakTap.def windowsapp.lib user32.lib advapi32.lib ole32.lib comctl32.lib /OUT:TrayTweakTap.dll || exit /b 1
)
cl %FLAGS% TrayTweak.cpp /link /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS user32.lib advapi32.lib shell32.lib /OUT:TrayTweak.exe || exit /b 1
cl %FLAGS% TrayTweakPanel.cpp /link /MANIFEST:EMBED /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS user32.lib gdi32.lib advapi32.lib ole32.lib shell32.lib comctl32.lib version.lib dwmapi.lib uxtheme.lib /OUT:TrayTweakPanel.exe || exit /b 1
cl %FLAGS% TrayTweakVolume.cpp /link /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib advapi32.lib ole32.lib shell32.lib version.lib dwmapi.lib /OUT:TrayTweakVolume.exe || exit /b 1
rem The setup embeds the files above. asInvoker keeps Windows from asking for admin
rem rights just because the name contains "Setup".
rc /nologo /fo TrayTweakSetup.res TrayTweakSetup.rc || exit /b 1
cl %FLAGS% TrayTweakSetup.cpp TrayTweakSetup.res /link /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS user32.lib advapi32.lib ole32.lib shell32.lib /OUT:TrayTweakSetup.exe || exit /b 1
del /q TrayTweakSetup.obj TrayTweakSetup.res TrayTweakVolume.obj TrayTweakTap.obj TrayTweak.obj TrayTweakPanel.obj TrayTweakTap.exp TrayTweakTap.lib 2>nul
echo BUILD OK
