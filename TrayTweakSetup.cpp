// TrayTweakSetup.exe - installs TrayTweak for the current user, without admin rights.
//
//   TrayTweakSetup.exe              install or update, then open the panel
//   TrayTweakSetup.exe /uninstall   remove TrayTweak (Apps > Installed apps runs this)
//
// The program files are embedded as resources (see TrayTweakSetup.rc). They go to
// %LOCALAPPDATA%\Programs\TrayTweak, with a Start menu shortcut and an entry in
// Installed apps. Settings in HKCU\Software\TrayTweak are kept on update.
#include "TrayCommon.h"

#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TrayTweak";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"TrayTweak";

struct Payload {
    int id;
    const wchar_t *file;
};
// Resource ids match TrayTweakSetup.rc.
constexpr Payload kFiles[] = {
    {101, L"TrayTweak.exe"},
    {102, L"TrayTweakPanel.exe"},
    {103, L"TrayTweakTap.dll"},
    {104, L"TrayTweakVolume.exe"},
    {105, L"LICENSE.txt"},
};

bool g_turkish;

const wchar_t *Text(const wchar_t *english, const wchar_t *turkish) { return g_turkish ? turkish : english; }

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

std::wstring InstallDir() { return KnownFolder(FOLDERID_UserProgramFiles) + L"\\TrayTweak"; }
std::wstring ShortcutPath() { return KnownFolder(FOLDERID_Programs) + L"\\TrayTweak.lnk"; }

std::wstring SelfPath() {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return length && length < MAX_PATH ? std::wstring(path, length) : std::wstring();
}

void Message(const wchar_t *text, UINT flags = MB_OK | MB_ICONINFORMATION) { MessageBoxW(nullptr, text, L"TrayTweak", flags); }

bool Ask(const wchar_t *text) { return MessageBoxW(nullptr, text, L"TrayTweak", MB_YESNO | MB_ICONQUESTION) == IDYES; }

// The mixer and the panel hold their exe open, so they are closed before files are replaced.
void CloseRunning() {
    if (HWND mixer = FindWindowW(kVolumeWindowClass, nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(mixer, &pid);
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(mixer, WM_CLOSE, 0, 0);
        if (process) {
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
        }
    }
    if (HWND panel = FindWindowW(L"TrayTweakPanelWindow", nullptr)) PostMessageW(panel, WM_CLOSE, 0, 0);
    Sleep(300);
}

// A DLL loaded into Explorer can't be overwritten, but it can be renamed. The renamed
// copy is deleted the next time setup runs, when Explorer no longer has it.
bool WriteFileFromResource(std::wstring const &dir, Payload const &item) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(item.id), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const void *data = loaded ? LockResource(loaded) : nullptr;
    DWORD size = resource ? SizeofResource(nullptr, resource) : 0;
    if (!data || !size) return false;

    std::wstring target = dir + L"\\" + item.file;
    HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::wstring old = target + L".old";
        DeleteFileW(old.c_str());
        if (!MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
        file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(file, data, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

bool CreateShortcut(std::wstring const &target, std::wstring const &link) {
    IShellLinkW *shell = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) return false;
    shell->SetPath(target.c_str());
    std::wstring dir = target.substr(0, target.find_last_of(L'\\'));
    shell->SetWorkingDirectory(dir.c_str());
    shell->SetDescription(L"TrayTweak settings");
    IPersistFile *persist = nullptr;
    bool ok = SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&persist))) && SUCCEEDED(persist->Save(link.c_str(), TRUE));
    if (persist) persist->Release();
    shell->Release();
    return ok;
}

void SetString(HKEY key, const wchar_t *name, std::wstring const &value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

void SetDword(HKEY key, const wchar_t *name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
}

void RegisterUninstall(std::wstring const &dir) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    SetString(key, L"DisplayName", L"TrayTweak");
    SetString(key, L"DisplayVersion", kVersion);
    SetString(key, L"Publisher", L"Popcorn");
    SetString(key, L"URLInfoAbout", L"https://github.com/izgara/TrayTweak");
    SetString(key, L"InstallLocation", dir);
    SetString(key, L"DisplayIcon", dir + L"\\TrayTweakPanel.exe");
    SetString(key, L"UninstallString", L"\"" + dir + L"\\TrayTweakSetup.exe\" /uninstall");
    SetDword(key, L"NoModify", 1);
    SetDword(key, L"NoRepair", 1);
    SetDword(key, L"EstimatedSize", 1200);
    RegCloseKey(key);
}

void RestartExplorer() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    if (tray) GetWindowThreadProcessId(tray, &pid);
    HANDLE process = pid ? OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid) : nullptr;
    if (process) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 5000);
        CloseHandle(process);
    }
    // Explorer usually restarts by itself; start it if it didn't.
    for (int i = 0; i < 20 && !FindWindowW(L"Shell_TrayWnd", nullptr); i++) Sleep(250);
    if (!FindWindowW(L"Shell_TrayWnd", nullptr)) ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
    for (int i = 0; i < 40 && !FindWindowW(L"Shell_TrayWnd", nullptr); i++) Sleep(250);
}

bool ExplorerHasOurDll(std::wstring const &dir) {
    std::wstring old = dir + L"\\TrayTweakTap.dll.old";
    return GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES;
}

int Install() {
    std::wstring dir = InstallDir();
    bool update = GetFileAttributesW((dir + L"\\TrayTweak.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
    std::wstring prompt = update ? Text(L"Update TrayTweak to version ", L"TrayTweak şu sürüme güncellensin mi: ")
                                 : Text(L"Install TrayTweak ", L"TrayTweak kurulsun mu: ");
    prompt += kVersion;
    prompt += Text(L"?\n\nIt is installed for your account only, in:\n", L"?\n\nSadece senin hesabın için şuraya kurulur:\n");
    prompt += dir;
    if (!Ask(prompt.c_str())) return 1;

    CloseRunning();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    DeleteFileW((dir + L"\\TrayTweakTap.dll.old").c_str());  // from an earlier update, if Explorer let go of it

    for (auto const &item : kFiles) {
        if (!WriteFileFromResource(dir, item)) {
            std::wstring error = Text(L"Could not write ", L"Şu dosya yazılamadı: ");
            error += item.file;
            Message(error.c_str(), MB_OK | MB_ICONERROR);
            return 2;
        }
    }
    std::wstring self = SelfPath(), setupCopy = dir + L"\\TrayTweakSetup.exe";
    if (_wcsicmp(self.c_str(), setupCopy.c_str()) != 0) CopyFileW(self.c_str(), setupCopy.c_str(), FALSE);

    CreateShortcut(dir + L"\\TrayTweakPanel.exe", ShortcutPath());
    RegisterUninstall(dir);

    // Keep "Start with Windows" if it was on, now pointing at the installed copy.
    if (!ReadString(HKEY_CURRENT_USER, kRunKey, kRunValue).empty()) {
        std::wstring value = L"\"" + dir + L"\\TrayTweak.exe\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    }

    // The new DLL only loads into a fresh Explorer. Without a restart, TrayTweak.exe isn't
    // run: Explorer may still hold a TrayTweakTap.dll from another folder, and loading a
    // second copy next to it would run both. The installed copy takes over at next sign-in.
    bool restarted = false;
    if (ExplorerHasOurDll(dir) &&
        Ask(Text(L"TrayTweak was updated. Explorer has to restart to load the new version: the taskbar disappears for a few seconds. Restart it now?",
                 L"TrayTweak güncellendi. Yeni sürümün yüklenmesi için Explorer'ın yeniden başlaması gerekiyor; görev çubuğu birkaç saniye kaybolur. Şimdi yeniden başlatılsın mı?"))) {
        RestartExplorer();
        restarted = true;
    }
    if (restarted) {
        ShellExecuteW(nullptr, L"open", (dir + L"\\TrayTweak.exe").c_str(), L"--interactive", dir.c_str(), SW_SHOWNORMAL);
    } else if (ReadSetting(kVolumeMixer) && !FindWindowW(kVolumeWindowClass, nullptr)) {
        ShellExecuteW(nullptr, L"open", (dir + L"\\TrayTweakVolume.exe").c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    }
    ShellExecuteW(nullptr, L"open", (dir + L"\\TrayTweakPanel.exe").c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    return 0;
}

int Uninstall() {
    if (!Ask(Text(L"Remove TrayTweak and its settings?\n\nExplorer restarts once so the taskbar goes back to normal.",
                  L"TrayTweak ve ayarları kaldırılsın mı?\n\nGörev çubuğunun eski haline dönmesi için Explorer bir kez yeniden başlar."))) {
        return 1;
    }
    CloseRunning();
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    // Show the overflow arrow again if TrayTweak hid it.
    if (ReadSetting(kHideArrow)) {
        DWORD one = 1;
        RegSetKeyValueW(HKEY_CURRENT_USER, kArrowKey, kArrowValue, REG_DWORD, &one, sizeof(one));
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, kSettingsRoot);
    RegDeleteKeyW(HKEY_CURRENT_USER, kSettingsRoot);
    DeleteFileW(ShortcutPath().c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);

    // Unloads the DLL, so the folder can be deleted.
    RestartExplorer();

    // This exe lives in the folder, so a short-lived cmd deletes it after we exit.
    std::wstring dir = InstallDir();
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring cmd = std::wstring(system) + L"\\cmd.exe";
    std::wstring args = L"\"" + cmd + L"\" /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + dir + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(cmd.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, system, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    Message(Text(L"TrayTweak was removed.\n\nThe one machine-wide value it may have set can be removed from an admin prompt:\nreg delete \"HKLM\\Software\\Microsoft\\XAML\\Debug\" /v DisableCompositionDiag /f",
                 L"TrayTweak kaldırıldı.\n\nAyarlamış olabileceği tek sistem değeri yönetici komut isteminden silinebilir:\nreg delete \"HKLM\\Software\\Microsoft\\XAML\\Debug\" /v DisableCompositionDiag /f"));
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    g_turkish = UseTurkish();
    int result = wcsstr(commandLine, L"/uninstall") ? Uninstall() : Install();
    CoUninitialize();
    return result;
}
