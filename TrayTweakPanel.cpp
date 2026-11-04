// TrayTweakPanel.exe - small window for the TrayTweak options and for choosing which
// tray icons stay on the taskbar. Only runs while it is open. Changes apply immediately.
#include "TrayCommon.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>

#include <algorithm>
#include <map>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kClassName[] = L"TrayTweakPanelWindow";
constexpr wchar_t kUsbGuid[] = L"guid:{7820ae78-23e3-4229-82c1-e41cb67d5b9c}";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"TrayTweak";
constexpr COLORREF kDarkBack = RGB(32, 32, 32);
constexpr COLORREF kDarkText = RGB(240, 240, 240);
constexpr UINT kLoaderFinished = WM_APP + 1;

enum class OptionKind { Setting, Arrow, ShowAll, Autostart, Language };

struct Option {
    OptionKind kind;
    const wchar_t *setting;
    const wchar_t *english;
    const wchar_t *turkish;
};

constexpr Option kOptions[] = {
    {OptionKind::Setting, kHideVolume, L"Hide the volume icon", L"Ses simgesini gizle"},
    {OptionKind::Setting, kHideMicrophone, L"Hide the microphone privacy icon", L"Mikrofon gizlilik simgesini gizle"},
    {OptionKind::Setting, kHideLanguage, L"Hide the language indicator", L"Dil göstergesini gizle"},
    {OptionKind::Arrow, kHideArrow, L"Hide the arrow (^): hidden icons are fully gone", L"Oku (^) gizle: gizlenen simgeler tamamen kaybolur"},
    {OptionKind::ShowAll, kShowAllIcons, L"Always show icons, also for new apps", L"Simgeleri her zaman göster, yeni uygulamalar dahil"},
    {OptionKind::Autostart, nullptr, L"Start with Windows", L"Windows ile başlat"},
    {OptionKind::Language, nullptr, L"Türkçe arayüz", L"Türkçe arayüz"},
};

struct Row {
    std::wstring key;
    std::wstring name;
    std::wstring path;
    std::vector<std::wstring> subKeys;
    bool shown = false;
};

std::vector<Row> g_rows;
std::set<std::wstring> g_hidden;
HWND g_window;
HWND g_options;
HWND g_list;
HFONT g_font;
HBRUSH g_backBrush;
bool g_dark;
bool g_turkish;
bool g_filling;
bool g_restart;

const wchar_t *Text(const wchar_t *english, const wchar_t *turkish) { return g_turkish ? turkish : english; }

int Scale(int value) { return MulDiv(value, GetDpiForSystem(), 96); }

std::wstring ModuleSibling(const wchar_t *file) {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return {};
    slash[1] = L'\0';
    return std::wstring(path) + file;
}

// Runs TrayTweak.exe, which loads the DLL if it isn't loaded yet, and reports failure.
DWORD WINAPI LoaderThread(void *) {
    std::wstring exe = ModuleSibling(L"TrayTweak.exe");
    std::wstring commandLine = L"\"" + exe + L"\" --interactive";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    DWORD code = 1;
    if (CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
    PostMessageW(g_window, kLoaderFinished, code, 0);
    return 0;
}

void StartLoader() {
    HANDLE thread = CreateThread(nullptr, 0, LoaderThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}

// Paths under known folders are stored as "{FOLDER-GUID}\rest\app.exe".
std::wstring ResolvePath(std::wstring const &stored) {
    if (stored.empty() || stored[0] != L'{') return stored;
    size_t close = stored.find(L'}');
    if (close == std::wstring::npos) return stored;
    GUID folder;
    if (FAILED(CLSIDFromString(stored.substr(0, close + 1).c_str(), &folder))) return stored;
    PWSTR base = nullptr;
    std::wstring result = stored;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &base))) result = std::wstring(base) + stored.substr(close + 1);
    CoTaskMemFree(base);
    return result;
}

std::wstring FriendlyName(std::wstring const &path) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size) {
        std::vector<BYTE> data(size);
        struct Translation {
            WORD language, codePage;
        } *translation = nullptr;
        UINT length = 0;
        if (GetFileVersionInfoW(path.c_str(), 0, size, data.data()) &&
            VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&translation), &length) &&
            length >= sizeof(Translation)) {
            wchar_t query[64];
            swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription", translation->language, translation->codePage);
            wchar_t *description = nullptr;
            if (VerQueryValueW(data.data(), query, reinterpret_cast<void **>(&description), &length) && length > 1 && description[0]) {
                return description;
            }
        }
    }
    size_t slash = path.find_last_of(L'\\');
    std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = file.find_last_of(L'.');
    return dot == std::wstring::npos ? file : file.substr(0, dot);
}

void LoadRows() {
    g_hidden = LoadHidden();
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kTrayRoot, 0, KEY_READ, &root) != ERROR_SUCCESS) return;
    std::map<std::wstring, Row> rows;
    wchar_t name[256];
    for (DWORD i = 0;; i++) {
        DWORD length = ARRAYSIZE(name);
        if (RegEnumKeyExW(root, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring stored = ReadString(root, name, L"ExecutablePath");
        if (stored.empty()) continue;
        std::wstring key = EntryKey(stored, ReadString(root, name, L"IconGuid"));
        Row &row = rows[key];
        row.key = key;
        row.subKeys.push_back(name);
        DWORD promoted = 0, size = sizeof(promoted);
        if (RegGetValueW(root, name, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &promoted, &size) == ERROR_SUCCESS && promoted == 1) row.shown = true;
        // Old entries of removed or moved apps have no file. They still get updated,
        // but only a copy that exists decides whether the app gets a row.
        std::wstring path = ResolvePath(stored);
        if (row.path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) row.path = path;
    }
    RegCloseKey(root);

    for (auto &[key, row] : rows) {
        if (row.path.empty()) continue;
        if (key == kUsbGuid) {
            row.name = Text(L"Safely Remove Hardware (USB)", L"Donanımı güvenle kaldır (USB)");
        } else if (key.rfind(L"guid:", 0) == 0) {
            row.name = Text(L"Windows icon", L"Windows simgesi");
        } else {
            row.name = FriendlyName(row.path);
        }
        if (g_hidden.count(key)) row.shown = false;
        g_rows.push_back(std::move(row));
    }
    std::sort(g_rows.begin(), g_rows.end(), [](Row const &a, Row const &b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
}

void WritePromoted(Row const &row, bool shown) {
    DWORD value = shown ? 1 : 0;
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kTrayRoot, 0, KEY_SET_VALUE, &root) != ERROR_SUCCESS) return;
    for (auto const &subKey : row.subKeys) {
        RegSetKeyValueW(root, subKey.c_str(), L"IsPromoted", REG_DWORD, &value, sizeof(value));
    }
    RegCloseKey(root);
}

void ApplyRow(Row &row, bool shown) {
    row.shown = shown;
    if (shown) {
        g_hidden.erase(row.key);
    } else {
        g_hidden.insert(row.key);
    }
    SaveHidden(g_hidden);
    WritePromoted(row, shown);
}

bool AutostartEnabled() { return !ReadString(HKEY_CURRENT_USER, kRunKey, kRunValue).empty(); }

void SetAutostart(bool on) {
    if (on) {
        std::wstring value = L"\"" + ModuleSibling(L"TrayTweak.exe") + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }
}

bool OptionState(Option const &option) {
    switch (option.kind) {
        case OptionKind::Autostart:
            return AutostartEnabled();
        case OptionKind::Language:
            return g_turkish;
        default:
            return ReadSetting(option.setting);
    }
}

void ApplyOption(Option const &option, bool on) {
    switch (option.kind) {
        case OptionKind::Setting:
            WriteSetting(option.setting, on);
            if (on) StartLoader();
            break;
        case OptionKind::Arrow: {
            WriteSetting(option.setting, on);
            DWORD value = on ? 0 : 1;
            RegSetKeyValueW(HKEY_CURRENT_USER, kArrowKey, kArrowValue, REG_DWORD, &value, sizeof(value));
            break;
        }
        case OptionKind::ShowAll:
            WriteSetting(option.setting, on);
            if (on) {
                g_filling = true;
                for (int i = 0; i < (int)g_rows.size(); i++) {
                    if (g_hidden.count(g_rows[i].key)) continue;
                    g_rows[i].shown = true;
                    WritePromoted(g_rows[i], true);
                    ListView_SetCheckState(g_list, i, TRUE);
                }
                g_filling = false;
                StartLoader();
            }
            break;
        case OptionKind::Autostart:
            SetAutostart(on);
            break;
        case OptionKind::Language: {
            std::wstring value = on ? L"tr" : L"en";
            RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsRoot, kLanguage, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
            g_restart = true;
            PostMessageW(g_window, WM_CLOSE, 0, 0);
            break;
        }
    }
}

HICON RowIcon(Row const &row) {
    if (row.key == kUsbGuid) {
        HICON icon = nullptr;
        if (ExtractIconExW(L"hotplug.dll", 0, nullptr, &icon, 1) && icon) return icon;
    }
    SHFILEINFOW info{};
    if (SHGetFileInfoW(row.path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON)) return info.hIcon;
    return nullptr;
}

HWND CreateLabel(const wchar_t *text, int x, int y, int width, int height) {
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, width, height, g_window, nullptr, nullptr, nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return label;
}

HWND CreateCheckList(int x, int y, int width, int height) {
    HWND list = CreateWindowExW(0, WC_LISTVIEWW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER,
                                x, y, width, height, g_window, nullptr, nullptr, nullptr);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    if (g_dark) {
        SetWindowTheme(list, L"DarkMode_Explorer", nullptr);
        ListView_SetBkColor(list, kDarkBack);
        ListView_SetTextBkColor(list, kDarkBack);
        ListView_SetTextColor(list, kDarkText);
    }
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = width - GetSystemMetrics(SM_CXVSCROLL) - Scale(4);
    ListView_InsertColumn(list, 0, &column);
    return list;
}

void FillOptions() {
    g_filling = true;
    for (int i = 0; i < (int)ARRAYSIZE(kOptions); i++) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = const_cast<wchar_t *>(Text(kOptions[i].english, kOptions[i].turkish));
        ListView_InsertItem(g_options, &item);
        ListView_SetCheckState(g_options, i, OptionState(kOptions[i]));
    }
    g_filling = false;
}

void FillApps() {
    int size = GetSystemMetrics(SM_CXSMICON);
    HIMAGELIST images = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, (int)g_rows.size(), 0);
    ListView_SetImageList(g_list, images, LVSIL_SMALL);
    g_filling = true;
    for (int i = 0; i < (int)g_rows.size(); i++) {
        int image = -1;
        if (HICON icon = RowIcon(g_rows[i])) {
            image = ImageList_AddIcon(images, icon);
            DestroyIcon(icon);
        }
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = i;
        item.pszText = g_rows[i].name.data();
        item.iImage = image;
        ListView_InsertItem(g_list, &item);
        ListView_SetCheckState(g_list, i, g_rows[i].shown);
    }
    g_filling = false;
}

// A check box changed. Returns the new state if this notification is about one.
bool CheckChanged(NMLISTVIEW const *change, bool *on) {
    if (g_filling || !(change->uChanged & LVIF_STATE) || !((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK)) return false;
    // An item being inserted goes from no check box at all to unchecked.
    if (!(change->uOldState & LVIS_STATEIMAGEMASK)) return false;
    *on = ((change->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
    return true;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            g_window = window;
            RECT client;
            GetClientRect(window, &client);
            int margin = Scale(12);
            int width = client.right - 2 * margin;
            int labelHeight = Scale(22);
            int y = margin;
            CreateLabel(Text(L"Options", L"Seçenekler"), margin, y, width, labelHeight);
            y += labelHeight;
            g_options = CreateCheckList(margin, y, width, Scale(170));
            FillOptions();
            RECT itemRect{};
            ListView_GetItemRect(g_options, 0, &itemRect, LVIR_BOUNDS);
            int optionsHeight = (itemRect.bottom - itemRect.top) * (int)ARRAYSIZE(kOptions) + Scale(8);
            MoveWindow(g_options, margin, y, width, optionsHeight, TRUE);
            y += optionsHeight + Scale(10);
            CreateLabel(Text(L"Icons to show on the taskbar", L"Görev çubuğunda gösterilecek simgeler"), margin, y, width, labelHeight);
            y += labelHeight;
            g_list = CreateCheckList(margin, y, width, client.bottom - y - margin);
            FillApps();
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            if (g_dark) {
                SetTextColor(reinterpret_cast<HDC>(wParam), kDarkText);
                SetBkColor(reinterpret_cast<HDC>(wParam), kDarkBack);
                return reinterpret_cast<LRESULT>(g_backBrush);
            }
            break;
        case WM_NOTIFY: {
            auto *header = reinterpret_cast<NMHDR *>(lParam);
            if (header->code == LVN_ITEMCHANGED) {
                auto *change = reinterpret_cast<NMLISTVIEW *>(lParam);
                bool on = false;
                if (CheckChanged(change, &on) && change->iItem >= 0) {
                    if (header->hwndFrom == g_options && change->iItem < (int)ARRAYSIZE(kOptions)) {
                        ApplyOption(kOptions[change->iItem], on);
                    } else if (header->hwndFrom == g_list && change->iItem < (int)g_rows.size() && g_rows[change->iItem].shown != on) {
                        ApplyRow(g_rows[change->iItem], on);
                    }
                }
            }
            break;
        }
        case kLoaderFinished:
            if (wParam != 0) {
                MessageBoxW(window,
                            Text(L"The icons could not be hidden. If you declined the admin prompt, try again. Details are in TrayTweak.log.",
                                 L"Simgeler gizlenemedi. Yönetici iznini reddettiysen tekrar dene. Ayrıntılar TrayTweak.log dosyasında."),
                            L"TrayTweak", MB_OK | MB_ICONWARNING);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool SystemUsesDarkApps() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

bool UseTurkish() {
    std::wstring language = ReadString(HKEY_CURRENT_USER, kSettingsRoot, kLanguage);
    if (!language.empty()) return language == L"tr";
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    if (HWND existing = FindWindowW(kClassName, nullptr)) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return 0;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);

    g_turkish = UseTurkish();
    LoadRows();
    g_dark = SystemUsesDarkApps();
    g_backBrush = g_dark ? CreateSolidBrush(kDarkBack) : GetSysColorBrush(COLOR_WINDOW);
    g_font = CreateFontW(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = g_backBrush;
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, Scale(400), Scale(640)};
    AdjustWindowRect(&rect, style, FALSE);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HWND window = CreateWindowExW(0, kClassName, L"TrayTweak", style, (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
                                  (GetSystemMetrics(SM_CYSCREEN) - height) / 2, width, height, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    if (g_dark) {
        BOOL enable = TRUE;
        DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &enable, sizeof(enable));
    }
    ShowWindow(window, show);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            DestroyWindow(window);
            continue;
        }
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_restart) {
        std::wstring self = ModuleSibling(L"TrayTweakPanel.exe");
        ShellExecuteW(nullptr, L"open", self.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return 0;
}
