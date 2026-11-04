// TrayTweak.exe - runs at sign-in, or when the panel asks, and exits.
//   1. Applies the tray settings chosen in TrayTweakPanel.exe.
//   2. If an option needs it, loads TrayTweakTap.dll into the taskbar's thread in
//      explorer.exe. Running it again is harmless: the DLL only starts once.
//
// --interactive     started from the panel, so a UAC prompt may be shown if needed
// --elevated-setup  writes the one machine-wide value below, then exits
#include "TrayCommon.h"

#include <shellapi.h>

#include <cstdio>
#include <cwchar>

namespace {

wchar_t g_dir[MAX_PATH];

void Log(const char *format, ...) {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sTrayTweak.log", g_dir);
    char line[300];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = sprintf_s(line, "%02d:%02d:%02d [loader] ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, format);
    int m = vsprintf_s(line + n, sizeof(line) - n - 2, format, args);
    va_end(args);
    if (m < 0) return;
    n += m;
    line[n++] = '\r';
    line[n++] = '\n';
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(f, line, n, &written, nullptr);
    CloseHandle(f);
}

// Keeps the log from growing forever: it starts over once it passes 64 KB.
void TrimLog() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sTrayTweak.log", g_dir);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &data) && data.nFileSizeLow > 64 * 1024) DeleteFileW(path);
}

void ApplyTraySettings() {
    if (ReadSetting(kHideArrow)) {
        DWORD zero = 0;
        LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, kArrowKey, kArrowValue, REG_DWORD, &zero, sizeof(zero));
        if (status != ERROR_SUCCESS) Log("arrow setting failed: %ld", status);
    }

    HKEY key;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kTrayRoot, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (status != ERROR_SUCCESS) {
        Log("tray settings not opened: %ld", status);
        return;
    }
    auto const hidden = LoadHidden();
    bool const showAll = ReadSetting(kShowAllIcons);
    int total = 0, changed = 0, hiddenCount = 0;
    wchar_t name[256];
    for (DWORD i = 0;; i++) {
        DWORD length = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        total++;
        bool hide = hidden.count(EntryKey(ReadString(key, name, L"ExecutablePath"), ReadString(key, name, L"IconGuid"))) > 0;
        if (hide) hiddenCount++;
        // Without "show all", icons the user didn't hide keep whatever Windows has.
        if (!hide && !showAll) continue;
        DWORD wanted = hide ? 0 : 1;
        DWORD value = 0, size = sizeof(value);
        LSTATUS read = RegGetValueW(key, name, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &value, &size);
        if (read != ERROR_SUCCESS || value != wanted) {
            if (RegSetKeyValueW(key, name, L"IsPromoted", REG_DWORD, &wanted, sizeof(wanted)) == ERROR_SUCCESS) changed++;
        }
    }
    RegCloseKey(key);
    Log("tray: %d apps, %d changed, %d hidden", total, changed, hiddenCount);
}

bool DiagnosticsValueIsSet() {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE, kDiagnosticsKey, kDiagnosticsValue, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value == 1;
}

LSTATUS SetDiagnosticsValue() {
    HKEY key;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kDiagnosticsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status == ERROR_SUCCESS) {
        DWORD value = 1;
        status = RegSetValueExW(key, kDiagnosticsValue, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
        RegCloseKey(key);
    }
    return status;
}

// XAML's composition diagnostics can corrupt the heap when two UI threads add a
// visual at once. This XAML debug value keeps them from being created. It has no
// effect on anything else: normal apps never connect diagnostics.
bool EnsureDiagnosticsValue(bool interactive) {
    if (DiagnosticsValueIsSet()) return true;
    LSTATUS status = SetDiagnosticsValue();
    if (status == ERROR_SUCCESS) return true;
    if (!interactive) {
        Log("the machine-wide value isn't set and can't be set without admin rights (%ld). Open the panel once to set it.", status);
        return false;
    }
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe;
    info.lpParameters = L"--elevated-setup";
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        Log("admin prompt was declined: %lu", GetLastError());
        return false;
    }
    WaitForSingleObject(info.hProcess, 60000);
    CloseHandle(info.hProcess);
    return DiagnosticsValueIsSet();
}

// The XAML taskbar appears a little after the taskbar window during sign-in.
HWND WaitForTaskbarUi() {
    for (int i = 0; i < 240; i++) {
        if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
            HWND ui = FindWindowExW(tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
            if (ui) return ui;
        }
        Sleep(500);
    }
    return nullptr;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    DWORD length = GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    if (!length || length >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(g_dir, L'\\');
    if (!slash) return 1;
    slash[1] = L'\0';

    if (wcsstr(commandLine, L"--elevated-setup")) return SetDiagnosticsValue() == ERROR_SUCCESS ? 0 : 3;

    TrimLog();
    ApplyTraySettings();

    if (!ReadSetting(kHideVolume) && !ReadSetting(kHideMicrophone) && !ReadSetting(kHideLanguage) && !ReadSetting(kShowAllIcons)) {
        Log("nothing needs the DLL");
        return 0;
    }
    if (!EnsureDiagnosticsValue(wcsstr(commandLine, L"--interactive") != nullptr)) return 3;

    HWND ui = WaitForTaskbarUi();
    if (!ui) {
        Log("taskbar not found");
        return 4;
    }

    wchar_t dll[MAX_PATH];
    swprintf_s(dll, L"%sTrayTweakTap.dll", g_dir);
    HMODULE module = LoadLibraryW(dll);
    FARPROC proc = module ? GetProcAddress(module, "HookProc") : nullptr;
    if (!proc) {
        Log("DLL not loaded: %lu", GetLastError());
        return 5;
    }

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, L"Local\\TrayTweak.Ready");
    DWORD thread = GetWindowThreadProcessId(ui, nullptr);
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, reinterpret_cast<HOOKPROC>(proc), module, thread);
    if (!hook) {
        Log("hook failed: %lu", GetLastError());
        return 6;
    }

    DWORD_PTR result;
    if (!SendMessageTimeoutW(ui, RegisterWindowMessageW(L"TrayTweak.Start"), 0, 0, SMTO_ABORTIFHUNG, 10000, &result)) {
        Log("taskbar did not respond: %lu", GetLastError());
    }

    // Removing the hook is safe: the DLL has pinned itself inside Explorer.
    DWORD wait = WaitForSingleObject(ready, 30000);
    Log(wait == WAIT_OBJECT_0 ? "done" : "timed out waiting for the taskbar");
    UnhookWindowsHookEx(hook);
    return wait == WAIT_OBJECT_0 ? 0 : 7;
}
