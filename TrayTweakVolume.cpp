// TrayTweakVolume.exe - a small volume mixer in the tray, in place of the Windows volume
// icon. Click the icon for the device and per-app sliders, right-click it to switch the
// output device. While the flyout is closed only the tray icon, a hidden window and two
// audio callbacks are alive: the meter and session timers run only while it is open.
#include "TrayCommon.h"

#include <audioclient.h>
#include <audiopolicy.h>
#include <dwmapi.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <map>

using Microsoft::WRL::ComPtr;

namespace {

constexpr GUID kIconGuid = {0x5b0c7a1e, 0x3f42, 0x4d6b, {0x9a, 0x1c, 0x27, 0x8e, 0x41, 0xd3, 0x6f, 0x90}};
constexpr GUID kPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
constexpr PROPERTYKEY kFriendlyNameKey = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kVolumeChanged = WM_APP + 2;
constexpr UINT kDeviceChanged = WM_APP + 3;
constexpr UINT_PTR kMeterTimer = 1;
constexpr UINT_PTR kScanTimer = 2;
constexpr UINT kDeviceMenuBase = 100;

// Segoe Fluent Icons code points (numeric on purpose, see README build notes).
constexpr wchar_t kGlyphVolume0 = 0xE992, kGlyphVolume1 = 0xE993, kGlyphVolume2 = 0xE994, kGlyphVolume3 = 0xE995;
constexpr wchar_t kGlyphMute = 0xE74F, kGlyphBell = 0xE7ED, kGlyphChevron = 0xE70D, kGlyphSettings = 0xE713, kGlyphMixer = 0xE9E9;

// Undocumented but stable since Windows 7; every device switcher uses it.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

struct App {
    std::wstring key;
    std::wstring name;
    HICON icon = nullptr;
    bool system = false;
    std::vector<ComPtr<ISimpleAudioVolume>> volumes;
    std::vector<ComPtr<IAudioMeterInformation>> meters;
    float peak = 0;
};

struct Palette {
    COLORREF back, footer, line, text, dim, track, thumb, thumbBorder;
};

struct Metrics {
    int width, pad, rowHeight, top, appsTop, footer, height;
};

HWND g_window;
UINT g_taskbarCreated;
bool g_turkish, g_dark, g_lightTaskbar, g_useGuid = true, g_menuOpen, g_iconAdded;
std::wstring g_iconFace = L"Segoe Fluent Icons";
HFONT g_font, g_boldFont, g_glyphFont, g_smallGlyphFont;
COLORREF g_accent;
ComPtr<IMMDeviceEnumerator> g_enumerator;
ComPtr<IMMDevice> g_device;
ComPtr<IAudioEndpointVolume> g_endpoint;
ComPtr<IAudioMeterInformation> g_masterMeter;
std::wstring g_deviceName;
std::vector<App> g_apps;
int g_visibleApps;
float g_masterPeak;
int g_dragRow = -1;
DWORD g_hiddenAt;
HICON g_trayIcon;
int g_trayPercent = -1, g_trayMuted = -1;
std::wstring g_trayDevice;
RECT g_deviceRect, g_settingsRect, g_mixerRect;
std::vector<std::wstring> g_menuDevices;

const wchar_t *Text(const wchar_t *english, const wchar_t *turkish) { return g_turkish ? turkish : english; }

int Scale(int value) { return MulDiv(value, GetDpiForSystem(), 96); }

float Clamp01(float value) { return value < 0 ? 0 : value > 1 ? 1 : value; }

std::wstring ModuleSibling(const wchar_t *file) {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return {};
    slash[1] = L'\0';
    return std::wstring(path) + file;
}

// ---------------------------------------------------------------- audio callbacks
// Both are static objects, so reference counting is a formality. They only post.

class EndpointCallback final : public IAudioEndpointVolumeCallback {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IAudioEndpointVolumeCallback)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        PostMessageW(g_window, kVolumeChanged, 0, 0);
        return S_OK;
    }
} g_endpointCallback;

class DeviceCallback final : public IMMNotificationClient {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) PostMessageW(g_window, kDeviceChanged, 0, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
} g_deviceCallback;

// ---------------------------------------------------------------- device and sessions

std::wstring DeviceName(IMMDevice *device) {
    ComPtr<IPropertyStore> store;
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && SUCCEEDED(store->GetValue(kFriendlyNameKey, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

std::wstring DeviceId(IMMDevice *device) {
    LPWSTR id = nullptr;
    std::wstring result;
    if (device && SUCCEEDED(device->GetId(&id)) && id) result = id;
    CoTaskMemFree(id);
    return result;
}

void UnbindDevice() {
    if (g_endpoint) g_endpoint->UnregisterControlChangeNotify(&g_endpointCallback);
    g_endpoint.Reset();
    g_masterMeter.Reset();
    g_device.Reset();
    g_deviceName.clear();
}

void BindDevice() {
    UnbindDevice();
    if (FAILED(g_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_device))) return;
    g_device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(g_endpoint.GetAddressOf()));
    g_device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(g_masterMeter.GetAddressOf()));
    if (g_endpoint) g_endpoint->RegisterControlChangeNotify(&g_endpointCallback);
    g_deviceName = DeviceName(g_device.Get());
}

float MasterLevel() {
    float value = 0;
    if (g_endpoint) g_endpoint->GetMasterVolumeLevelScalar(&value);
    return value;
}

bool MasterMuted() {
    BOOL muted = FALSE;
    if (g_endpoint) g_endpoint->GetMute(&muted);
    return muted != FALSE;
}

std::wstring ProcessPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH * 2];
    DWORD size = ARRAYSIZE(path);
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) result.assign(path, size);
    CloseHandle(process);
    return result;
}

void ClearApps() {
    for (auto &app : g_apps) {
        if (app.icon) DestroyIcon(app.icon);
    }
    g_apps.clear();
}

// Sessions are grouped by program, so a browser with several audio processes gets one
// slider. Names and icons of programs already listed are kept between rescans.
void ScanApps() {
    struct Cached {
        std::wstring name;
        HICON icon;
        float peak;
    };
    std::map<std::wstring, Cached> cache;
    for (auto &app : g_apps) cache[app.key] = {app.name, app.icon, app.peak};
    g_apps.clear();

    ComPtr<IAudioSessionManager2> manager;
    ComPtr<IAudioSessionEnumerator> sessions;
    int count = 0;
    if (g_device &&
        SUCCEEDED(g_device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(manager.GetAddressOf()))) &&
        SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
        sessions->GetCount(&count);
    }

    struct Session {
        ComPtr<IAudioSessionControl> control;
        ComPtr<ISimpleAudioVolume> volume;
        ComPtr<IAudioMeterInformation> meter;
        bool system;
        DWORD pid;
    };
    std::vector<Session> live;
    std::map<DWORD, std::wstring> paths;  // pid -> exe path, for every pid that has a session
    for (int i = 0; i < count; i++) {
        Session s{};
        AudioSessionState state;
        if (FAILED(sessions->GetSession(i, &s.control)) || FAILED(s.control->GetState(&state)) || state == AudioSessionStateExpired) continue;
        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(s.control.As(&control2)) || FAILED(s.control.As(&s.volume))) continue;
        s.control.As(&s.meter);
        s.system = control2->IsSystemSoundsSession() == S_OK;
        control2->GetProcessId(&s.pid);
        if (!s.system && s.pid == GetCurrentProcessId()) continue;
        if (!s.system && !paths.count(s.pid)) paths[s.pid] = ProcessPath(s.pid);
        live.push_back(std::move(s));
    }

    // Some players hand the audio to a helper process (Spotifast plays through its child
    // soloist.exe), so a child is filed under its parent when the parent has a session too.
    std::map<DWORD, DWORD> parents;
    if (paths.size() > 1) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{sizeof(entry)};
            for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
                if (paths.count(entry.th32ProcessID)) parents[entry.th32ProcessID] = entry.th32ParentProcessID;
            }
            CloseHandle(snapshot);
        }
    }
    auto owner = [&](DWORD pid) {
        for (int depth = 0; depth < 4; depth++) {
            auto parent = parents.find(pid);
            if (parent == parents.end() || parent->second == pid || !paths.count(parent->second)) break;
            pid = parent->second;
        }
        return pid;
    };

    std::map<std::wstring, size_t> index;
    for (auto &s : live) {
        auto &control = s.control;
        auto &volume = s.volume;
        auto &meter = s.meter;
        bool system = s.system;
        DWORD pid = system ? s.pid : owner(s.pid);
        std::wstring path = system ? std::wstring() : paths[pid];
        std::wstring key = system ? L"system" : path.empty() ? L"pid:" + std::to_wstring(pid) : Lower(path);

        auto found = index.find(key);
        if (found == index.end()) {
            index[key] = g_apps.size();
            App app;
            app.key = key;
            app.system = system;
            auto cached = cache.find(key);
            if (cached != cache.end()) {
                app.name = cached->second.name;
                app.icon = cached->second.icon;
                app.peak = cached->second.peak;
                cache.erase(cached);
            } else if (system) {
                app.name = Text(L"System sounds", L"Sistem sesleri");
            } else if (!path.empty()) {
                app.name = FriendlyName(path);
                SHFILEINFOW info{};
                if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON)) app.icon = info.hIcon;
            } else {
                LPWSTR display = nullptr;
                if (SUCCEEDED(control->GetDisplayName(&display)) && display && display[0] && display[0] != L'@') app.name = display;
                CoTaskMemFree(display);
                if (app.name.empty()) app.name = Text(L"App ", L"Uygulama ") + std::to_wstring(pid);
            }
            g_apps.push_back(std::move(app));
            found = index.find(key);
        }
        App &app = g_apps[found->second];
        app.volumes.push_back(volume);
        if (meter) app.meters.push_back(meter);
    }
    for (auto &[key, cached] : cache) {
        if (cached.icon) DestroyIcon(cached.icon);
    }
    std::sort(g_apps.begin(), g_apps.end(), [](App const &a, App const &b) {
        if (a.system != b.system) return b.system;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

// Row 0 is the device, the rest are apps.
int RowCount() { return 1 + g_visibleApps; }

float RowLevel(int row) {
    if (row == 0) return MasterLevel();
    float value = 0;
    auto &app = g_apps[row - 1];
    if (!app.volumes.empty()) app.volumes[0]->GetMasterVolume(&value);
    return value;
}

bool RowMuted(int row) {
    if (row == 0) return MasterMuted();
    BOOL muted = FALSE;
    auto &app = g_apps[row - 1];
    if (!app.volumes.empty()) app.volumes[0]->GetMute(&muted);
    return muted != FALSE;
}

void SetRowLevel(int row, float value) {
    if (row == 0) {
        if (g_endpoint) g_endpoint->SetMasterVolumeLevelScalar(value, nullptr);
        return;
    }
    for (auto &volume : g_apps[row - 1].volumes) volume->SetMasterVolume(value, nullptr);
}

void SetRowMute(int row, bool muted) {
    if (row == 0) {
        if (g_endpoint) g_endpoint->SetMute(muted, nullptr);
        return;
    }
    for (auto &volume : g_apps[row - 1].volumes) volume->SetMute(muted, nullptr);
}

float ReadPeak(IAudioMeterInformation *meter) {
    float value = 0;
    if (meter) meter->GetPeakValue(&value);
    return value;
}

void UpdatePeaks() {
    auto smooth = [](float old, float now) { return now > old ? now : old * 0.8f; };
    g_masterPeak = smooth(g_masterPeak, ReadPeak(g_masterMeter.Get()));
    for (auto &app : g_apps) {
        float now = 0;
        for (auto &meter : app.meters) now = (std::max)(now, ReadPeak(meter.Get()));
        app.peak = smooth(app.peak, now);
    }
}

void SetDefaultDevice(std::wstring const &id) {
    ComPtr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), reinterpret_cast<void **>(policy.GetAddressOf())))) return;
    for (ERole role : {eConsole, eMultimedia, eCommunications}) policy->SetDefaultEndpoint(id.c_str(), role);
}

// ---------------------------------------------------------------- theme and tray icon

DWORD ReadDword(HKEY root, const wchar_t *key, const wchar_t *value, DWORD fallback) {
    DWORD data = fallback, size = sizeof(data);
    if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) return fallback;
    return data;
}

COLORREF Mix(COLORREF a, COLORREF b, int percentB) {
    auto channel = [&](int x, int y) { return (BYTE)((x * (100 - percentB) + y * percentB) / 100); };
    return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)), channel(GetBValue(a), GetBValue(b)));
}

HFONT MakeFont(const wchar_t *face, int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

void PickIconFont() {
    HDC dc = GetDC(nullptr);
    HFONT font = MakeFont(g_iconFace.c_str(), 16, FW_NORMAL);
    HGDIOBJ old = SelectObject(dc, font);
    wchar_t face[LF_FACESIZE] = {};
    GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, old);
    DeleteObject(font);
    ReleaseDC(nullptr, dc);
    if (_wcsicmp(face, g_iconFace.c_str()) != 0) g_iconFace = L"Segoe MDL2 Assets";
}

void LoadTheme() {
    constexpr wchar_t personalize[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    g_dark = ReadDword(HKEY_CURRENT_USER, personalize, L"AppsUseLightTheme", 1) == 0;
    g_lightTaskbar = ReadDword(HKEY_CURRENT_USER, personalize, L"SystemUsesLightTheme", 0) == 1;
    DWORD accent = ReadDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFD77800);
    g_accent = RGB(accent & 0xFF, (accent >> 8) & 0xFF, (accent >> 16) & 0xFF);
    if (g_dark) g_accent = Mix(g_accent, RGB(255, 255, 255), 35);

    for (HFONT font : {g_font, g_boldFont, g_glyphFont, g_smallGlyphFont}) {
        if (font) DeleteObject(font);
    }
    g_font = MakeFont(L"Segoe UI", Scale(14), FW_NORMAL);
    g_boldFont = MakeFont(L"Segoe UI", Scale(14), FW_SEMIBOLD);
    g_glyphFont = MakeFont(g_iconFace.c_str(), Scale(20), FW_NORMAL);
    g_smallGlyphFont = MakeFont(g_iconFace.c_str(), Scale(12), FW_NORMAL);

    if (g_window) {
        BOOL dark = g_dark;
        DwmSetWindowAttribute(g_window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
    // Popup menus follow this. uxtheme ordinal 135 is SetPreferredAppMode.
    using SetPreferredAppMode = int(WINAPI *)(int);
    using FlushMenuThemes = void(WINAPI *)();
    if (HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        auto setMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
        if (setMode) setMode(g_dark ? 2 : 3);
        if (flush) flush();
    }
}

Palette Colors() {
    if (g_dark) {
        return {RGB(44, 44, 44), RGB(36, 36, 36), RGB(62, 62, 62), RGB(255, 255, 255), RGB(170, 170, 170),
                RGB(154, 154, 154), RGB(69, 69, 69), RGB(88, 88, 88)};
    }
    return {RGB(249, 249, 249), RGB(238, 238, 238), RGB(224, 224, 224), RGB(26, 26, 26), RGB(96, 96, 96),
            RGB(137, 137, 137), RGB(255, 255, 255), RGB(208, 208, 208)};
}

wchar_t VolumeGlyph(float level, bool muted) {
    if (muted) return kGlyphMute;
    if (level <= 0.005f) return kGlyphVolume0;
    if (level < 0.34f) return kGlyphVolume1;
    if (level < 0.67f) return kGlyphVolume2;
    return kGlyphVolume3;
}

// Draws the glyph white on black and uses the brightness as alpha.
HICON MakeTrayIcon(wchar_t glyph) {
    int size = GetSystemMetrics(SM_CXSMICON);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    void *bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HICON icon = nullptr;
    if (color && bits) {
        HGDIOBJ oldBitmap = SelectObject(dc, color);
        HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, g_iconFace.c_str());
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT rect{0, 0, size, size};
        DrawTextW(dc, &glyph, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        BYTE ink = g_lightTaskbar ? 0 : 255;
        auto *pixels = static_cast<DWORD *>(bits);
        for (int i = 0; i < size * size; i++) {
            DWORD p = pixels[i];
            BYTE alpha = (BYTE)(std::max)({(p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF});
            BYTE c = (BYTE)(ink * alpha / 255);
            pixels[i] = ((DWORD)alpha << 24) | ((DWORD)c << 16) | ((DWORD)c << 8) | c;
        }
        SelectObject(dc, oldFont);
        DeleteObject(font);
        SelectObject(dc, oldBitmap);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
        ICONINFO iconInfo{TRUE, 0, 0, mask, color};
        icon = CreateIconIndirect(&iconInfo);
        DeleteObject(mask);
    }
    if (color) DeleteObject(color);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

NOTIFYICONDATAW IconData() {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = g_window;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP | (g_useGuid ? NIF_GUID : 0);
    data.guidItem = kIconGuid;
    data.uCallbackMessage = kTrayMessage;
    return data;
}

void UpdateTray(bool add, bool force = false) {
    float level = MasterLevel();
    bool muted = MasterMuted();
    int percent = (int)std::lround(level * 100);
    if (!add && !force && percent == g_trayPercent && (int)muted == g_trayMuted && g_deviceName == g_trayDevice) return;
    g_trayPercent = percent;
    g_trayMuted = muted;
    g_trayDevice = g_deviceName;

    NOTIFYICONDATAW data = IconData();
    data.hIcon = MakeTrayIcon(g_device ? VolumeGlyph(level, muted) : kGlyphMute);
    if (!g_device) {
        wcscpy_s(data.szTip, Text(L"No output device", L"Çıkış cihazı yok"));
    } else if (muted) {
        swprintf_s(data.szTip, L"%.100s: %s", g_deviceName.c_str(), Text(L"muted", L"sessiz"));
    } else {
        swprintf_s(data.szTip, L"%.100s: %d%%", g_deviceName.c_str(), percent);
    }

    if (add) {
        // A crashed earlier copy can leave its GUID registered: remove it and retry. If the
        // GUID belongs to an exe at another path, Windows refuses it, so go without.
        g_iconAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (!g_iconAdded && g_useGuid) {
            Shell_NotifyIconW(NIM_DELETE, &data);
            g_iconAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        }
        if (!g_iconAdded && g_useGuid && FindWindowW(L"Shell_TrayWnd", nullptr)) {
            g_useGuid = false;
            data.uFlags &= ~NIF_GUID;
            g_iconAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        }
        if (g_iconAdded) {
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }
    } else if (g_iconAdded) {
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    g_trayIcon = data.hIcon;
}

// ---------------------------------------------------------------- flyout layout

Metrics GetMetrics() {
    Metrics m;
    m.width = Scale(360);
    m.pad = Scale(16);
    m.rowHeight = Scale(60);
    m.top = Scale(6);
    m.appsTop = m.top + m.rowHeight + Scale(7);
    m.footer = Scale(44);
    int apps = g_visibleApps ? g_visibleApps * m.rowHeight : Scale(44);
    m.height = m.appsTop + apps + Scale(6) + m.footer;
    return m;
}

int RowTop(Metrics const &m, int row) { return row == 0 ? m.top : m.appsTop + (row - 1) * m.rowHeight; }

RECT SliderRect(Metrics const &m, int row) {
    int center = RowTop(m, row) + Scale(41);
    return {m.pad + Scale(54), center, m.width - m.pad - Scale(46), center};
}

RECT IconRect(Metrics const &m, int row) {
    int center = RowTop(m, row) + m.rowHeight / 2;
    return {m.pad, center - Scale(18), m.pad + Scale(36), center + Scale(18)};
}

int RowAt(Metrics const &m, int y) {
    for (int row = 0; row < RowCount(); row++) {
        int top = RowTop(m, row);
        if (y >= top && y < top + m.rowHeight) return row;
    }
    return -1;
}

enum class Hit { None, Slider, Icon, Device, Settings, Mixer };

Hit HitTest(POINT p, int *row) {
    Metrics m = GetMetrics();
    if (PtInRect(&g_deviceRect, p)) return Hit::Device;
    if (PtInRect(&g_settingsRect, p)) return Hit::Settings;
    if (PtInRect(&g_mixerRect, p)) return Hit::Mixer;
    *row = RowAt(m, p.y);
    if (*row < 0) return Hit::None;
    RECT icon = IconRect(m, *row);
    if (PtInRect(&icon, p)) return Hit::Icon;
    RECT slider = SliderRect(m, *row);
    if (p.x >= slider.left - Scale(12) && p.x <= slider.right + Scale(12) && std::abs(p.y - slider.top) <= Scale(14)) return Hit::Slider;
    return Hit::None;
}

void PositionFlyout() {
    Metrics base = GetMetrics();
    NOTIFYICONIDENTIFIER id{sizeof(id)};
    id.hWnd = g_window;
    id.uID = 1;
    if (g_useGuid) id.guidItem = kIconGuid;
    RECT icon{};
    if (FAILED(Shell_NotifyIconGetRect(&id, &icon))) {
        POINT cursor;
        GetCursorPos(&cursor);
        icon = {cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
    }
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&icon, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT work = monitor.rcWork;
    int gap = Scale(12);

    int fixed = base.appsTop + Scale(6) + base.footer;
    int room = (int)((work.bottom - work.top - 2 * gap - fixed) / base.rowHeight);
    g_visibleApps = (int)(std::min)((size_t)(std::max)(room, 1), g_apps.size());
    Metrics m = GetMetrics();

    int x = (icon.left + icon.right) / 2 - m.width / 2;
    x = (std::max)((int)work.left + gap, (std::min)(x, (int)work.right - m.width - gap));
    int y;
    if (icon.top >= work.bottom) {
        y = work.bottom - m.height - gap;
    } else if (icon.bottom <= work.top) {
        y = work.top + gap;
    } else {
        y = (icon.top + icon.bottom) / 2 - m.height / 2;
        y = (std::max)((int)work.top + gap, (std::min)(y, (int)work.bottom - m.height - gap));
        x = icon.left >= work.right ? work.right - m.width - gap : work.left + gap;
    }
    SetWindowPos(g_window, HWND_TOPMOST, x, y, m.width, m.height, SWP_NOACTIVATE);
}

// ---------------------------------------------------------------- painting

Gdiplus::Color ToColor(COLORREF c, BYTE alpha = 255) { return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c)); }

void DrawGlyph(HDC dc, wchar_t glyph, HFONT font, RECT rect, COLORREF color) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, &glyph, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void DrawSlider(HDC dc, RECT slider, float level, float peak, bool muted, bool active, Palette const &p) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float y = (float)slider.top, left = (float)slider.left, right = (float)slider.right;
    float x = left + (right - left) * level;
    float thickness = (float)Scale(4);
    auto line = [&](Gdiplus::Color color, float from, float to) {
        if (to <= from) return;
        Gdiplus::Pen pen(color, thickness);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&pen, from, y, to, y);
    };
    COLORREF fill = muted ? p.dim : g_accent;
    line(ToColor(p.track), left, right);
    line(ToColor(fill), left, x);
    if (!muted) line(ToColor(RGB(255, 255, 255), g_dark ? 120 : 110), left, left + (x - left) * Clamp01(peak));

    float outer = (float)Scale(10), inner = (float)Scale(active ? 7 : 5);
    Gdiplus::SolidBrush thumb(ToColor(p.thumb));
    g.FillEllipse(&thumb, x - outer, y - outer, 2 * outer, 2 * outer);
    Gdiplus::Pen border(ToColor(p.thumbBorder), 1.0f);
    g.DrawEllipse(&border, x - outer, y - outer, 2 * outer, 2 * outer);
    Gdiplus::SolidBrush dot(ToColor(fill));
    g.FillEllipse(&dot, x - inner, y - inner, 2 * inner, 2 * inner);
}

void DrawRow(HDC dc, Metrics const &m, int row, Palette const &p) {
    int top = RowTop(m, row);
    float level = RowLevel(row);
    bool muted = RowMuted(row);
    RECT icon = IconRect(m, row);
    App *app = row ? &g_apps[row - 1] : nullptr;

    if (app && app->icon) {
        int size = Scale(28);
        int x = (icon.left + icon.right - size) / 2, y = (icon.top + icon.bottom - size) / 2;
        DrawIconEx(dc, x, y, app->icon, size, size, 0, nullptr, DI_NORMAL);
        if (muted) {
            {
                Gdiplus::Graphics g(dc);
                Gdiplus::SolidBrush veil(ToColor(p.back, 160));
                g.FillRectangle(&veil, x, y, size, size);
            }
            DrawGlyph(dc, kGlyphMute, g_smallGlyphFont, {x + size / 2, y + size / 2, x + size + Scale(4), y + size + Scale(4)}, p.text);
        }
    } else {
        wchar_t glyph = row == 0 ? VolumeGlyph(level, muted) : muted ? kGlyphMute : app->system ? kGlyphBell : kGlyphVolume3;
        DrawGlyph(dc, glyph, g_glyphFont, icon, muted ? p.dim : p.text);
    }

    RECT name{m.pad + Scale(54) - Scale(10), top + Scale(9), m.width - m.pad, top + Scale(29)};
    if (row == 0) {
        std::wstring text = g_device ? g_deviceName : Text(L"No output device", L"Çıkış cihazı yok");
        SelectObject(dc, g_boldFont);
        RECT measure = name;
        DrawTextW(dc, text.c_str(), -1, &measure, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
        int chevron = Scale(18);
        if (measure.right > name.right - chevron) measure.right = name.right - chevron;
        RECT textRect{name.left, name.top, measure.right, name.bottom};
        SetTextColor(dc, p.text);
        DrawTextW(dc, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT glyphRect{measure.right + Scale(2), name.top + Scale(2), measure.right + chevron, name.bottom};
        DrawGlyph(dc, kGlyphChevron, g_smallGlyphFont, glyphRect, p.dim);
        g_deviceRect = {name.left - Scale(4), name.top - Scale(4), glyphRect.right + Scale(4), name.bottom + Scale(4)};
    } else {
        SelectObject(dc, g_font);
        SetTextColor(dc, p.text);
        DrawTextW(dc, app->name.c_str(), -1, &name, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    RECT slider = SliderRect(m, row);
    float peak = row == 0 ? g_masterPeak : app->peak;
    DrawSlider(dc, slider, level, row == 0 ? peak : peak, muted, g_dragRow == row, p);

    wchar_t value[8];
    swprintf_s(value, L"%d", (int)std::lround(level * 100));
    RECT valueRect{m.width - m.pad - Scale(36), slider.top - Scale(11), m.width - m.pad, slider.top + Scale(11)};
    SelectObject(dc, g_font);
    SetTextColor(dc, muted ? p.dim : p.text);
    DrawTextW(dc, value, -1, &valueRect, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
}

RECT DrawLink(HDC dc, wchar_t glyph, const wchar_t *text, int x, int centerY, bool alignRight, Palette const &p) {
    SelectObject(dc, g_font);
    RECT measure{0, 0, 0, 0};
    DrawTextW(dc, text, -1, &measure, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    int glyphWidth = Scale(24);
    int width = glyphWidth + measure.right;
    int left = alignRight ? x - width : x;
    DrawGlyph(dc, glyph, g_smallGlyphFont, {left, centerY - Scale(10), left + Scale(16), centerY + Scale(10)}, p.text);
    SelectObject(dc, g_font);
    SetTextColor(dc, p.text);
    RECT textRect{left + glyphWidth, centerY - Scale(10), left + width, centerY + Scale(10)};
    DrawTextW(dc, text, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    return {left - Scale(6), centerY - Scale(14), left + width + Scale(6), centerY + Scale(14)};
}

void Paint(HDC target) {
    RECT client;
    GetClientRect(g_window, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    HGDIOBJ oldFont = SelectObject(dc, g_font);
    Palette p = Colors();
    Metrics m = GetMetrics();

    HBRUSH back = CreateSolidBrush(p.back), footer = CreateSolidBrush(p.footer), line = CreateSolidBrush(p.line);
    FillRect(dc, &client, back);
    RECT footerRect{0, client.bottom - m.footer, client.right, client.bottom};
    FillRect(dc, &footerRect, footer);
    RECT footerLine{0, footerRect.top, client.right, footerRect.top + 1};
    FillRect(dc, &footerLine, line);
    RECT separator{m.pad, m.appsTop - Scale(4), client.right - m.pad, m.appsTop - Scale(3)};
    FillRect(dc, &separator, line);
    SetBkMode(dc, TRANSPARENT);

    for (int row = 0; row < RowCount(); row++) DrawRow(dc, m, row, p);
    if (g_visibleApps == 0) {
        RECT empty{m.pad, m.appsTop, client.right - m.pad, m.appsTop + Scale(44)};
        SelectObject(dc, g_font);
        SetTextColor(dc, p.dim);
        DrawTextW(dc, Text(L"No apps are playing sound", L"Ses çalan uygulama yok"), -1, &empty, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    int center = footerRect.top + m.footer / 2;
    g_settingsRect = DrawLink(dc, kGlyphSettings, Text(L"Sound settings", L"Ses ayarları"), m.pad, center, false, p);
    g_mixerRect = DrawLink(dc, kGlyphMixer, Text(L"Windows mixer", L"Windows karıştırıcısı"), client.right - m.pad, center, true, p);

    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    DeleteObject(back);
    DeleteObject(footer);
    DeleteObject(line);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

// ---------------------------------------------------------------- flyout behaviour

void ShowFlyout() {
    ScanApps();
    g_masterPeak = 0;
    for (auto &app : g_apps) app.peak = 0;
    PositionFlyout();
    ShowWindow(g_window, SW_SHOW);
    SetForegroundWindow(g_window);
    InvalidateRect(g_window, nullptr, FALSE);
    SetTimer(g_window, kMeterTimer, 33, nullptr);
    SetTimer(g_window, kScanTimer, 1000, nullptr);
}

void HideFlyout() {
    if (!IsWindowVisible(g_window)) return;
    KillTimer(g_window, kMeterTimer);
    KillTimer(g_window, kScanTimer);
    if (g_dragRow >= 0) ReleaseCapture();
    g_dragRow = -1;
    ShowWindow(g_window, SW_HIDE);
    g_hiddenAt = GetTickCount();
    ClearApps();
    g_visibleApps = 0;
}

void Rescan() {
    size_t before = g_apps.size();
    ScanApps();
    if (g_apps.size() != before || (size_t)g_visibleApps > g_apps.size()) PositionFlyout();
    InvalidateRect(g_window, nullptr, FALSE);
}

void DragTo(int x) {
    Metrics m = GetMetrics();
    RECT slider = SliderRect(m, g_dragRow);
    float value = Clamp01((float)(x - slider.left) / (float)(slider.right - slider.left));
    value = std::round(value * 100) / 100;
    if (value > 0 && RowMuted(g_dragRow)) SetRowMute(g_dragRow, false);
    SetRowLevel(g_dragRow, value);
    InvalidateRect(g_window, nullptr, FALSE);
}

void OpenUri(const wchar_t *uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }

enum MenuCommand : UINT { kSoundSettings = 1, kWindowsMixer, kTrayTweakSettings, kExit };

void ShowMenu(POINT at, bool fromFlyout) {
    HMENU menu = CreatePopupMenu();
    g_menuDevices.clear();
    std::wstring current = DeviceId(g_device.Get());
    ComPtr<IMMDeviceCollection> devices;
    UINT count = 0;
    if (SUCCEEDED(g_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) devices->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, &device))) continue;
        std::wstring id = DeviceId(device.Get());
        std::wstring name = DeviceName(device.Get());
        AppendMenuW(menu, MF_STRING | (id == current ? MF_CHECKED : 0), kDeviceMenuBase + (UINT)g_menuDevices.size(), name.c_str());
        g_menuDevices.push_back(id);
    }
    if (g_menuDevices.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, Text(L"No output device", L"Çıkış cihazı yok"));
    if (!fromFlyout) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kSoundSettings, Text(L"Sound settings", L"Ses ayarları"));
        AppendMenuW(menu, MF_STRING, kWindowsMixer, Text(L"Windows volume mixer", L"Windows ses karıştırıcısı"));
        AppendMenuW(menu, MF_STRING, kTrayTweakSettings, Text(L"TrayTweak settings", L"TrayTweak ayarları"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kExit, Text(L"Exit", L"Çıkış"));
    }

    g_menuOpen = true;
    SetForegroundWindow(g_window);
    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | (fromFlyout ? TPM_LEFTALIGN | TPM_TOPALIGN : TPM_RIGHTALIGN | TPM_BOTTOMALIGN);
    UINT command = (UINT)TrackPopupMenuEx(menu, flags, at.x, at.y, g_window, nullptr);
    g_menuOpen = false;
    PostMessageW(g_window, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (command >= kDeviceMenuBase && command < kDeviceMenuBase + g_menuDevices.size()) {
        SetDefaultDevice(g_menuDevices[command - kDeviceMenuBase]);
        PostMessageW(g_window, kDeviceChanged, 0, 0);
    } else if (command == kSoundSettings) {
        OpenUri(L"ms-settings:sound");
    } else if (command == kWindowsMixer) {
        OpenUri(L"ms-settings:apps-volume");
    } else if (command == kTrayTweakSettings) {
        std::wstring panel = ModuleSibling(L"TrayTweakPanel.exe");
        ShellExecuteW(nullptr, L"open", panel.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (command == kExit) {
        DestroyWindow(g_window);
    }
    if (fromFlyout && IsWindowVisible(g_window) && GetForegroundWindow() != g_window) HideFlyout();
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_taskbarCreated && g_taskbarCreated) {
        UpdateTray(true);
        return 0;
    }
    switch (message) {
        case kTrayMessage:
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (IsWindowVisible(window)) {
                        HideFlyout();
                    } else if (GetTickCount() - g_hiddenAt > 400) {
                        ShowFlyout();
                    }
                    break;
                case WM_MBUTTONUP:
                    if (g_endpoint) g_endpoint->SetMute(!MasterMuted(), nullptr);
                    break;
                case WM_CONTEXTMENU:
                    HideFlyout();
                    ShowMenu({GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)}, false);
                    break;
            }
            return 0;
        case kVolumeChanged:
            UpdateTray(false);
            if (IsWindowVisible(window)) InvalidateRect(window, nullptr, FALSE);
            return 0;
        case kDeviceChanged:
            BindDevice();
            UpdateTray(false, true);
            if (IsWindowVisible(window)) Rescan();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && !g_menuOpen) HideFlyout();
            return 0;
        case WM_TIMER:
            if (wParam == kMeterTimer) {
                UpdatePeaks();
                InvalidateRect(window, nullptr, FALSE);
            } else if (wParam == kScanTimer && g_dragRow < 0) {
                Rescan();
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            Paint(dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(window, &p);
            int row = -1;
            Hit hit = HitTest(p, &row);
            bool hand = hit == Hit::Icon || hit == Hit::Device || hit == Hit::Settings || hit == Hit::Mixer;
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int row = -1;
            switch (HitTest(p, &row)) {
                case Hit::Slider:
                    g_dragRow = row;
                    SetCapture(window);
                    DragTo(p.x);
                    break;
                case Hit::Icon:
                    SetRowMute(row, !RowMuted(row));
                    InvalidateRect(window, nullptr, FALSE);
                    break;
                case Hit::Device: {
                    POINT at{g_deviceRect.left, g_deviceRect.bottom};
                    ClientToScreen(window, &at);
                    ShowMenu(at, true);
                    break;
                }
                case Hit::Settings:
                    HideFlyout();
                    OpenUri(L"ms-settings:sound");
                    break;
                case Hit::Mixer:
                    HideFlyout();
                    OpenUri(L"ms-settings:apps-volume");
                    break;
                case Hit::None:
                    break;
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g_dragRow >= 0) DragTo(GET_X_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            if (g_dragRow >= 0) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            g_dragRow = -1;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &p);
            int row = RowAt(GetMetrics(), p.y);
            if (row < 0) row = 0;
            float step = (float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 0.02f;
            float value = Clamp01(std::round((RowLevel(row) + step) * 100) / 100);
            if (value > 0 && RowMuted(row)) SetRowMute(row, false);
            SetRowLevel(row, value);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) HideFlyout();
            return 0;
        case WM_SETTINGCHANGE:
            if (lParam && wcscmp(reinterpret_cast<const wchar_t *>(lParam), L"ImmersiveColorSet") == 0) {
                LoadTheme();
                UpdateTray(false, true);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            LoadTheme();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY: {
            HideFlyout();
            NOTIFYICONDATAW data = IconData();
            Shell_NotifyIconW(NIM_DELETE, &data);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    if (FindWindowW(kVolumeWindowClass, nullptr)) return 0;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplus = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus, &gdiplusInput, nullptr) != Gdiplus::Ok) return 1;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&g_enumerator)))) return 1;

    g_turkish = UseTurkish();
    PickIconFont();
    LoadTheme();

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_DROPSHADOW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kVolumeWindowClass;
    RegisterClassExW(&windowClass);
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kVolumeWindowClass, L"TrayTweak Volume", WS_POPUP, 0, 0, 0, 0,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) return 1;
    BOOL dark = g_dark;
    DwmSetWindowAttribute(g_window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // If this was started elevated, Explorer runs at a lower integrity level and its
    // tray messages would be dropped without these.
    ChangeWindowMessageFilterEx(g_window, kTrayMessage, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_window, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
    BindDevice();
    g_enumerator->RegisterEndpointNotificationCallback(&g_deviceCallback);
    UpdateTray(true);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_enumerator->UnregisterEndpointNotificationCallback(&g_deviceCallback);
    UnbindDevice();
    ClearApps();
    g_enumerator.Reset();
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    Gdiplus::GdiplusShutdown(gdiplus);
    CoUninitialize();
    return 0;
}
