// Shared by TrayTweak.exe, TrayTweakPanel.exe and TrayTweakTap.dll: settings and tray entry keys.
#pragma once
#include <windows.h>

#include <set>
#include <string>
#include <vector>

constexpr wchar_t kVersion[] = L"1.1.0";
constexpr wchar_t kTrayRoot[] = L"Control Panel\\NotifyIconSettings";
constexpr wchar_t kSettingsRoot[] = L"Software\\TrayTweak";
constexpr wchar_t kArrowKey[] = L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\TrayNotify";
constexpr wchar_t kArrowValue[] = L"SystemTrayChevronVisibility";
constexpr wchar_t kDiagnosticsKey[] = L"Software\\Microsoft\\XAML\\Debug";
constexpr wchar_t kDiagnosticsValue[] = L"DisableCompositionDiag";

constexpr wchar_t kHideVolume[] = L"HideVolume";
constexpr wchar_t kHideMicrophone[] = L"HideMicrophone";
constexpr wchar_t kHideLanguage[] = L"HideLanguage";
constexpr wchar_t kHideArrow[] = L"HideArrow";
constexpr wchar_t kShowAllIcons[] = L"ShowAllIcons";
constexpr wchar_t kHiddenApps[] = L"HiddenApps";
constexpr wchar_t kLanguage[] = L"Language";
constexpr wchar_t kVolumeMixer[] = L"VolumeMixer";
constexpr wchar_t kVolumeWindowClass[] = L"TrayTweakVolumeWindow";
constexpr wchar_t kTaskbarGlass[] = L"TaskbarGlass";
constexpr wchar_t kMenuGlass[] = L"MenuGlass";
constexpr wchar_t kGlassColor[] = L"GlassColor";      // 0xRRGGBB
constexpr wchar_t kGlassOpacity[] = L"GlassOpacity";  // 0-100, tint strength over the blur
constexpr DWORD kDefaultGlassColor = 0x304E73;
constexpr DWORD kDefaultGlassOpacity = 50;

inline DWORD ReadSettingDword(const wchar_t *name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsRoot, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return fallback;
    return value;
}

inline std::wstring Lower(std::wstring text) {
    if (!text.empty()) {
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), (int)text.size(), text.data(),
                      (int)text.size(), nullptr, nullptr, 0);
    }
    return text;
}

inline std::wstring ReadString(HKEY key, const wchar_t *subKey, const wchar_t *value) {
    DWORD size = 0;
    if (RegGetValueW(key, subKey, value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < 2) return {};
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, subKey, value, RRF_RT_REG_SZ, nullptr, text.data(), &size) != ERROR_SUCCESS) return {};
    text.resize(wcsnlen(text.c_str(), text.size()));
    return text;
}

// All options are off until the user turns them on in the panel.
inline bool ReadSetting(const wchar_t *name) {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsRoot, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value != 0;
}

inline bool WriteSetting(const wchar_t *name, bool on) {
    DWORD value = on ? 1 : 0;
    return RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsRoot, name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

// The key stays the same when an app updates and moves to a new versioned folder,
// so it uses the file name. Explorer hosts several system icons under one exe, and
// those are told apart by GUID.
inline std::wstring EntryKey(std::wstring const &exePath, std::wstring const &guid) {
    size_t slash = exePath.find_last_of(L'\\');
    std::wstring file = Lower(slash == std::wstring::npos ? exePath : exePath.substr(slash + 1));
    if (file == L"explorer.exe" && !guid.empty()) return L"guid:" + Lower(guid);
    return file;
}

inline std::set<std::wstring> LoadHidden() {
    std::set<std::wstring> result;
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsRoot, kHiddenApps, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < 2) {
        return result;
    }
    std::vector<wchar_t> data(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsRoot, kHiddenApps, RRF_RT_REG_MULTI_SZ, nullptr, data.data(), &size) != ERROR_SUCCESS) {
        return result;
    }
    for (const wchar_t *entry = data.data(); *entry; entry += wcslen(entry) + 1) result.insert(Lower(entry));
    return result;
}

inline bool UseTurkish() {
    std::wstring language = ReadString(HKEY_CURRENT_USER, kSettingsRoot, kLanguage);
    if (!language.empty()) return language == L"tr";
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH;
}

// The file description from the version resource, or the file name. Needs version.lib.
inline std::wstring FriendlyName(std::wstring const &path) {
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

inline bool SaveHidden(std::set<std::wstring> const &hidden) {
    std::wstring data;
    for (auto const &entry : hidden) {
        data += entry;
        data.push_back(L'\0');
    }
    data.push_back(L'\0');
    return RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsRoot, kHiddenApps, REG_MULTI_SZ, data.data(),
                           (DWORD)(data.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}
