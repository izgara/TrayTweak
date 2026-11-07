// TrayTweakTap.dll - runs inside explorer.exe. Hides the Windows volume icon, the
// microphone privacy icon and the language indicator from the Windows 11 taskbar tray.
//
// How the icons are recognised (by glyph, so it works in every display language)
// and the layout fix for the hidden item come from the "Taskbar tray system icon
// tweaks" Windhawk mod by m417z, and the XAML diagnostics attach sequence from
// "Windows 11 Taskbar Styler". Both are GPL-3.0, and so is this file.
//
// To stay light, XAML diagnostics is only connected long enough to find the three
// tray elements. After that it is disconnected and its element references are
// released, so what stays in Explorer is a few ordinary XAML event handlers and one
// registry wait that only fires when a new app registers a tray icon. There are no
// dedicated threads, timers or hooks left behind.
#include <unknwn.h>
#include <windows.h>
#include "TrayCommon.h"
#include <ocidl.h>
#include <xamlOM.h>
#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.h>

#include <commctrl.h>

#include <atomic>
#include <cstdio>
#include <unordered_set>
#include <vector>

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = wux::Controls;
namespace wuxm = wux::Media;
namespace wuxs = wux::Shapes;
using FE = wux::FrameworkElement;

namespace {

// {5B0F8C3E-6D1A-4E7B-9C42-1F7A3D2E8B61}
constexpr CLSID kClsidTap = {0x5b0f8c3e, 0x6d1a, 0x4e7b, {0x9c, 0x42, 0x1f, 0x7a, 0x3d, 0x2e, 0x8b, 0x61}};
constexpr GUID kIidTestHooks = {0x735941a2, 0x3ee3, 0x495a, {0x8d, 0xa9, 0x97, 0x26, 0x27, 0x00, 0x30, 0x75}};

// Not declared by xamlOM.h. Releases the reference diagnostics keeps per element.
struct IXamlDiagnosticsTestHooks : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE UnregisterInstance(InstanceHandle handle) = 0;
};

HMODULE g_module;
DWORD g_uiThread;
HANDLE g_foundControlCenter;
HANDLE g_foundMainStack;
HANDLE g_foundLanguageStack;
HANDLE g_foundBackground;
std::atomic<bool> g_taskbarGlass;
std::atomic<bool> g_menuGlass;
std::atomic<DWORD> g_glassColor;
std::atomic<DWORD> g_glassOpacity;
bool g_glassHooksInstalled;
bool g_settingFill;
std::atomic<bool> g_hideVolume;
std::atomic<bool> g_hideMicrophone;
std::atomic<bool> g_hideLanguage;
std::atomic<bool> g_showAllIcons;
std::atomic<bool> g_started;
std::atomic<bool> g_advising;
std::atomic<bool> g_advisePending;

IXamlDiagnostics *g_diagnostics;
IXamlDiagnosticsTestHooks *g_testHooks;
IVisualTreeServiceCallback *g_watcher;

SRWLOCK g_releaseLock = SRWLOCK_INIT;

// Deliberately never destroyed: they must not run destructors during Explorer's
// shutdown, when the XAML objects behind them may already be gone.
auto &g_pendingRelease = *new std::unordered_set<InstanceHandle>();
auto &g_watchedText = *new std::unordered_set<void *>();
auto &g_attached = *new std::unordered_set<void *>();
auto &g_hiddenLanguageIcons = *new std::unordered_set<void *>();
struct Roots {
    winrt::weak_ref<FE> controlCenter;
    winrt::weak_ref<FE> mainStack;
    winrt::weak_ref<FE> languageStack;
    std::vector<winrt::weak_ref<wuxs::Shape>> backgrounds;  // one per taskbar (monitor)
};
auto &g_roots = *new Roots();
struct UiQueue {
    winrt::Windows::System::DispatcherQueue queue{nullptr};
};
UiQueue *g_uiQueue;

void Log(const char *format, ...) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (!len || len >= MAX_PATH - 4) return;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"TrayTweak.log");

    char line[300];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = sprintf_s(line, "%02d:%02d:%02d [explorer] ", t.wHour, t.wMinute, t.wSecond);
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

void SignalReady() {
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\TrayTweak.Ready");
    if (ready) {
        SetEvent(ready);
        CloseHandle(ready);
    }
}

wchar_t Glyph(winrt::hstring const &text) { return text.size() == 1 ? text[0] : L'\0'; }

bool IsVolumeGlyph(wchar_t c) {
    switch (c) {
        case 0xE74F:  // Mute
        case 0xE992:  // Volume0
        case 0xE993:  // Volume1
        case 0xE994:  // Volume2
        case 0xE995:  // Volume3
        case 0xEA85:  // VolumeDisabled
        case 0xEBC5:  // VolumeBars
            return true;
    }
    return false;
}

bool IsMicrophoneGlyph(wchar_t c) {
    switch (c) {
        case 0xE361:  // Private use, the current privacy icon
        case 0xE720:  // Microphone
        case 0xEC71:  // MicOn
        case 0xF47F:  // Microphone and location together
            return true;
    }
    return false;
}

template <typename Match>
FE Find(FE const &root, Match const &match, int depth) {
    if (depth <= 0) return nullptr;
    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(root, i).try_as<FE>();
        if (!child) continue;
        if (match(child)) return child;
        if (auto found = Find(child, match, depth - 1)) return found;
    }
    return nullptr;
}

// Calls fn for each system icon below root. Doesn't descend into the icons.
template <typename Fn>
void ForEachIcon(FE const &root, Fn const &fn, int depth = 14) {
    if (depth <= 0) return;
    int count = wuxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(root, i).try_as<FE>();
        if (!child) continue;
        if (child.Name() == L"SystemTrayIcon") {
            fn(child);
        } else {
            ForEachIcon(child, fn, depth - 1);
        }
    }
}

struct IconParts {
    FE content{nullptr};
    wuxc::TextBlock text{nullptr};
};

IconParts GetIconParts(FE const &icon) {
    IconParts parts;
    parts.content = Find(icon, [](FE const &e) { return winrt::get_class_name(e) == L"SystemTray.TextIconContent"; }, 7);
    if (parts.content) {
        // Other layers of the icon also hold an (empty) InnerTextBlock.
        auto base = Find(parts.content, [](FE const &e) { return e.Name() == L"Base"; }, 3);
        auto text = base ? Find(base, [](FE const &e) { return e.Name() == L"InnerTextBlock"; }, 2) : nullptr;
        if (text) parts.text = text.try_as<wuxc::TextBlock>();
    }
    return parts;
}

using ScanFn = void (*)(FE const &);

void ScanGuarded(FE const &root, ScanFn scan) {
    try {
        scan(root);
    } catch (...) {
        Log("scan failed: %08X", winrt::to_hresult().value);
    }
}

// The glyph can arrive after the element appears, so rescan when it changes.
void WatchText(wuxc::TextBlock const &text, FE const &root, ScanFn scan, const char *where) {
    if (!g_watchedText.insert(winrt::get_abi(text)).second) return;
    Log("%s icon glyph %04X", where, (unsigned)Glyph(text.Text()));
    auto weakRoot = winrt::make_weak(root);
    text.RegisterPropertyChangedCallback(
        wuxc::TextBlock::TextProperty(),
        [weakRoot, scan](wux::DependencyObject const &, wux::DependencyProperty const &) {
            if (auto strongRoot = weakRoot.get()) ScanGuarded(strongRoot, scan);
        });
}

void ScanMainStack(FE const &stack) {
    ForEachIcon(stack, [&](FE const &icon) {
        auto parts = GetIconParts(icon);
        if (!parts.text) return;
        WatchText(parts.text, stack, &ScanMainStack, "main stack");
        wchar_t glyph = Glyph(parts.text.Text());
        auto wanted = g_hideMicrophone && IsMicrophoneGlyph(glyph) ? wux::Visibility::Collapsed : wux::Visibility::Visible;
        if (icon.Visibility() != wanted) {
            icon.Visibility(wanted);
            Log("main stack glyph %04X: %s", (unsigned)glyph, wanted == wux::Visibility::Collapsed ? "hidden" : "shown");
        }
    });
}

// The language indicator (ENG TRQ). Only its view is collapsed: keyboard layouts and
// switching between them are not touched. A collapsed view can lose its content when
// the input method changes, so what was hidden here is remembered rather than looked
// up by content when it has to be shown again.
void ScanLanguageStack(FE const &stack) {
    bool hide = g_hideLanguage;
    ForEachIcon(stack, [&](FE const &icon) {
        void *key = winrt::get_abi(icon);
        if (!hide) {
            if (g_hiddenLanguageIcons.erase(key)) {
                icon.Visibility(wux::Visibility::Visible);
                Log("language indicator shown");
            }
            return;
        }
        auto content = Find(icon, [](FE const &e) {
            auto className = winrt::get_class_name(e);
            return className == L"SystemTray.LanguageTextIconContent" || className == L"SystemTray.LanguageImageIconContent";
        }, 6);
        if (content && icon.Visibility() != wux::Visibility::Collapsed) {
            icon.Visibility(wux::Visibility::Collapsed);
            g_hiddenLanguageIcons.insert(key);
            Log("language indicator hidden");
        }
    });
}

void SetMargin(FE const &item, double left, double right) {
    if (left == 0 && right == 0) {
        item.ClearValue(FE::MarginProperty());
    } else {
        item.Margin(wux::Thickness{left, 0, right, 0});
    }
}

// A hidden icon keeps its empty container, and the panel adds spacing for every
// child. Cancel that spacing with negative margins on the visible neighbours.
void UpdatePanelSpacing(wuxc::StackPanel const &panel) {
    double spacing = panel.Spacing();
    FE pending{nullptr};
    double pendingLeft = 0;
    int hiddenRun = 0;
    int count = wuxm::VisualTreeHelper::GetChildrenCount(panel);
    for (int i = 0; i < count; i++) {
        auto item = wuxm::VisualTreeHelper::GetChild(panel, i).try_as<FE>();
        if (!item) continue;
        auto icon = Find(item, [](FE const &e) { return e.Name() == L"SystemTrayIcon"; }, 2);
        auto control = icon ? icon.try_as<wuxc::Control>() : nullptr;
        if (!control) continue;
        if (!control.IsEnabled()) {
            hiddenRun++;
            continue;
        }
        if (pending) SetMargin(pending, pendingLeft, 0);
        pending = item;
        pendingLeft = -spacing * hiddenRun;
        hiddenRun = 0;
    }
    if (pending) SetMargin(pending, pendingLeft, -spacing * hiddenRun);
}

void ScanControlCenter(FE const &button) {
    wuxc::StackPanel panel{nullptr};
    ForEachIcon(button, [&](FE const &icon) {
        auto parts = GetIconParts(icon);
        if (!parts.text) return;
        WatchText(parts.text, button, &ScanControlCenter, "control center");
        wchar_t glyph = Glyph(parts.text.Text());
        bool hide = g_hideVolume && IsVolumeGlyph(glyph);
        bool hidden = parts.content.Visibility() == wux::Visibility::Collapsed;
        if (hide != hidden) {
            parts.content.Visibility(hide ? wux::Visibility::Collapsed : wux::Visibility::Visible);
            if (auto control = icon.try_as<wuxc::Control>()) control.IsEnabled(!hide);
            Log("control center glyph %04X: %s", (unsigned)glyph, hide ? "hidden" : "shown");
        }
        if (!panel) {
            auto item = wuxm::VisualTreeHelper::GetParent(icon);
            if (item) {
                auto parent = wuxm::VisualTreeHelper::GetParent(item);
                if (parent) panel = parent.try_as<wuxc::StackPanel>();
            }
        }
    });
    if (panel) UpdatePanelSpacing(panel);
}

// ---------------------------------------------------------------- glass
// The Windows 11 taskbar paints its own XAML background over its window. Making that
// fill transparent and asking DWM to blur behind the window gives a frosted glass
// taskbar. The same blur is given to the Open-Shell menu, which also lives in Explorer.

struct AccentPolicy {
    int state;
    int flags;
    DWORD gradient;  // 0xAABBGGRR
    int animation;
};
struct CompositionData {
    int attribute;
    void *data;
    SIZE_T size;
};
// Plain blur-behind (3) renders black on the Windows 11 taskbar; acrylic (4) works.
// With the tint at 0% the gradient mode (2) is used instead: fully clear, no blur.
constexpr int kAccentDisabled = 0, kAccentTransparent = 2, kAccentAcrylic = 4, kWcaAccentPolicy = 19;

DWORD GradientColor() {
    DWORD rgb = g_glassColor, opacity = g_glassOpacity > 100 ? 100 : g_glassOpacity.load();
    DWORD alpha = opacity * 255 / 100;
    return (alpha << 24) | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

void SetAccent(HWND window, bool on, DWORD gradient) {
    using SetWca = BOOL(WINAPI *)(HWND, CompositionData *);
    static auto set = reinterpret_cast<SetWca>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!set || !window) return;
    int state = !on ? kAccentDisabled : (gradient >> 24) == 0 ? kAccentTransparent : kAccentAcrylic;
    AccentPolicy policy{state, on ? 2 : 0, gradient, 0};
    CompositionData data{kWcaAccentPolicy, &policy, sizeof(policy)};
    set(window, &data);
}

BOOL CALLBACK ApplyToTaskbarWindow(HWND window, LPARAM) {
    wchar_t name[64];
    if (GetClassNameW(window, name, ARRAYSIZE(name)) &&
        (wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0)) {
        SetAccent(window, g_taskbarGlass, GradientColor());
    }
    return TRUE;
}

void ApplyTaskbarAccent() { EnumWindows(ApplyToTaskbarWindow, 0); }

void ApplyFill(wuxs::Shape const &fill) {
    g_settingFill = true;
    if (g_taskbarGlass) {
        fill.Fill(wuxm::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
    } else {
        fill.ClearValue(wuxs::Shape::FillProperty());
    }
    g_settingFill = false;
}

void ApplyGlass() {
    auto &list = g_roots.backgrounds;
    for (size_t i = 0; i < list.size();) {
        if (auto fill = list[i].get()) {
            ApplyFill(fill);
            i++;
        } else {
            list.erase(list.begin() + i);
        }
    }
    // Leave the window alone unless glass is on, or was on and must be undone.
    static bool accentWasOn;
    if (g_taskbarGlass || accentWasOn) ApplyTaskbarAccent();
    accentWasOn = g_taskbarGlass;
}

// Explorer can redo the window's composition on theme or display changes.
LRESULT CALLBACK TaskbarSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    switch (message) {
        case WM_DWMCOMPOSITIONCHANGED:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        case WM_DISPLAYCHANGE:
            if (g_taskbarGlass) ApplyTaskbarAccent();
            break;
    }
    return result;
}

void CALLBACK OnWindowShown(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD) {
    if (object != OBJID_WINDOW || !window || !g_menuGlass) return;
    wchar_t name[64];
    if (!GetClassNameW(window, name, ARRAYSIZE(name)) || wcscmp(name, L"OpenShell.CMenuContainer") != 0) return;
    // Open-Shell already paints its own tint, so only a faint one is added here.
    // Acrylic with zero alpha misrenders, hence the small value; at 0% it stays clear.
    DWORD gradient = GradientColor();
    SetAccent(window, true, (gradient >> 24) == 0 ? 0 : (gradient & 0x00FFFFFF) | 0x10000000);
}

// Runs once on the taskbar's thread, which owns Shell_TrayWnd and pumps messages.
void InstallGlassHooks() {
    if (g_glassHooksInstalled) return;
    g_glassHooksInstalled = true;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray && GetWindowThreadProcessId(tray, nullptr) == GetCurrentThreadId()) {
        SetWindowSubclass(tray, TaskbarSubclass, 1, 0);
    } else {
        Log("taskbar window is on another thread, no subclass");
    }
    if (!SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnWindowShown, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT)) {
        Log("menu glass hook failed: %lu", GetLastError());
    }
}

bool IsTaskbarBackground(FE const &element) {
    auto node = wuxm::VisualTreeHelper::GetParent(element);
    for (int i = 0; node && i < 4; i++) {
        if (winrt::get_class_name(node) == L"Taskbar.TaskbarBackground") return true;
        node = wuxm::VisualTreeHelper::GetParent(node);
    }
    return false;
}

void TrackBackground(wuxs::Shape const &fill) {
    for (auto const &weak : g_roots.backgrounds) {
        if (auto existing = weak.get(); existing && winrt::get_abi(existing) == winrt::get_abi(fill)) return;
    }
    g_roots.backgrounds.push_back(winrt::make_weak(fill));
    // Explorer restyles the fill on theme changes; put the transparent one back.
    fill.RegisterPropertyChangedCallback(wuxs::Shape::FillProperty(), [](wux::DependencyObject const &sender, wux::DependencyProperty const &) {
        if (g_settingFill || !g_taskbarGlass) return;
        if (auto shape = sender.try_as<wuxs::Shape>()) ApplyFill(shape);
    });
    if (g_taskbarGlass || g_menuGlass) InstallGlassHooks();
    if (g_taskbarGlass) ApplyGlass();
    Log("taskbar background found (%u)", (unsigned)g_roots.backgrounds.size());
}

void StartAdvise();

void AttachHandlers(FE const &element, ScanFn scan, const char *name) {
    void *key = winrt::get_abi(element);
    if (!g_attached.insert(key).second) return;
    Log("attached to %s", name);
    element.SizeChanged([scan](wf::IInspectable const &sender, wux::SizeChangedEventArgs const &) {
        if (auto self = sender.try_as<FE>()) ScanGuarded(self, scan);
    });
    // The tray was rebuilt, e.g. after a monitor reconnects. Find the new one.
    element.Unloaded([key, name](wf::IInspectable const &, wux::RoutedEventArgs const &) {
        Log("%s unloaded, reconnecting", name);
        g_attached.erase(key);
        g_watchedText.clear();
        StartAdvise();
    });
}

// Runs on the UI thread once diagnostics is disconnected, so no tree walk that
// could still use these references is in progress.
void FlushReleases() {
    std::vector<InstanceHandle> handles;
    AcquireSRWLockExclusive(&g_releaseLock);
    handles.assign(g_pendingRelease.begin(), g_pendingRelease.end());
    g_pendingRelease.clear();
    ReleaseSRWLockExclusive(&g_releaseLock);
    if (g_testHooks) {
        for (auto handle : handles) g_testHooks->UnregisterInstance(handle);
    }
    Log("released %u diagnostics references%s", (unsigned)handles.size(), g_testHooks ? "" : " (unavailable, kept)");
}

class Watcher : public winrt::implements<Watcher, IVisualTreeServiceCallback2, winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation, VisualElement element,
                                                 VisualMutationType mutationType) noexcept override {
        try {
            // The tray lives on the taskbar's thread. References can only be
            // released on the thread that owns them, so ignore all other threads.
            if (GetCurrentThreadId() != g_uiThread) return S_OK;

            AcquireSRWLockExclusive(&g_releaseLock);
            g_pendingRelease.insert(element.Handle);
            if (relation.Parent) g_pendingRelease.insert(relation.Parent);
            ReleaseSRWLockExclusive(&g_releaseLock);

            if (mutationType != Add) return S_OK;

            winrt::com_ptr<::IInspectable> raw;
            winrt::check_hresult(g_diagnostics->GetIInspectableFromHandle(element.Handle, raw.put()));
            auto frameworkElement = raw.try_as<FE>();
            if (!frameworkElement) return S_OK;

            auto name = frameworkElement.Name();
            if (name == L"ControlCenterButton") {
                if (!g_uiQueue) {
                    g_uiQueue = new UiQueue{
                        winrt::Windows::System::DispatcherQueue::GetForCurrentThread()};
                }
                g_roots.controlCenter = winrt::make_weak(frameworkElement);
                AttachHandlers(frameworkElement, &ScanControlCenter, "control center");
                ScanGuarded(frameworkElement, &ScanControlCenter);
                SetEvent(g_foundControlCenter);
            } else if (name == L"MainStack") {
                g_roots.mainStack = winrt::make_weak(frameworkElement);
                AttachHandlers(frameworkElement, &ScanMainStack, "main stack");
                ScanGuarded(frameworkElement, &ScanMainStack);
                SetEvent(g_foundMainStack);
            } else if (name == L"NonActivatableStack") {
                g_roots.languageStack = winrt::make_weak(frameworkElement);
                AttachHandlers(frameworkElement, &ScanLanguageStack, "language stack");
                ScanGuarded(frameworkElement, &ScanLanguageStack);
                SetEvent(g_foundLanguageStack);
            } else if ((name == L"BackgroundFill" || name == L"BackgroundStroke") && IsTaskbarBackground(frameworkElement)) {
                // The stroke is the thin line along the top edge of the taskbar.
                if (auto shape = frameworkElement.try_as<wuxs::Shape>()) {
                    TrackBackground(shape);
                    SetEvent(g_foundBackground);
                }
            }
        } catch (...) {
            Log("OnVisualTreeChange failed: %08X", winrt::to_hresult().value);
        }
        // Returning an error can stop further notifications.
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept override {
        return S_OK;
    }
};

// Advising from the UI thread can hang, so connect from a short-lived thread. It
// ends as soon as the tray elements were found and diagnostics is disconnected.
DWORD WINAPI AdviseThread(void *) {
    try {
        winrt::com_ptr<IVisualTreeService3> visualTree;
        HRESULT hr = g_diagnostics->QueryInterface(__uuidof(IVisualTreeService3), visualTree.put_void());
        if (SUCCEEDED(hr)) hr = visualTree->AdviseVisualTreeChange(g_watcher);
        if (FAILED(hr)) {
            Log("AdviseVisualTreeChange failed: %08X", hr);
        } else {
            Log("diagnostics connected");
            // No timeout: if the tray is being rebuilt while the monitor is off,
            // the new one may only appear when the monitor comes back.
            WaitForSingleObject(g_foundControlCenter, INFINITE);
            if (WaitForSingleObject(g_foundMainStack, 5000) != WAIT_OBJECT_0) Log("main stack not found");
            if (WaitForSingleObject(g_foundLanguageStack, 5000) != WAIT_OBJECT_0) Log("language stack not found");
            if (WaitForSingleObject(g_foundBackground, 5000) != WAIT_OBJECT_0) Log("taskbar background not found");
            hr = visualTree->UnadviseVisualTreeChange(g_watcher);
            Log("diagnostics disconnected: %08X", hr);
            if (g_uiQueue) {
                g_uiQueue->queue.TryEnqueue([] {
                    try {
                        FlushReleases();
                    } catch (...) {
                    }
                });
            }
        }
    } catch (...) {
        Log("AdviseThread failed: %08X", winrt::to_hresult().value);
    }
    g_advising = false;
    SignalReady();
    if (g_advisePending.exchange(false)) StartAdvise();
    return 0;
}

void StartAdvise() {
    if (!g_diagnostics || !g_watcher) return;
    if (g_advising.exchange(true)) {
        g_advisePending = true;
        return;
    }
    ResetEvent(g_foundControlCenter);
    ResetEvent(g_foundMainStack);
    ResetEvent(g_foundLanguageStack);
    ResetEvent(g_foundBackground);
    HANDLE thread = CreateThread(nullptr, 0, AdviseThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        g_advising = false;
        SignalReady();
    }
}

class Tap : public winrt::implements<Tap, IObjectWithSite, winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown *site) noexcept override {
        try {
            // The DLL stays for the life of Explorer, so there is no teardown.
            if (!site || g_diagnostics) return S_OK;
            winrt::check_hresult(site->QueryInterface(__uuidof(IXamlDiagnostics), reinterpret_cast<void **>(&g_diagnostics)));
            if (FAILED(g_diagnostics->QueryInterface(kIidTestHooks, reinterpret_cast<void **>(&g_testHooks)))) {
                g_testHooks = nullptr;
            }
            g_watcher = winrt::make_self<Watcher>().detach();
            StartAdvise();
            return S_OK;
        } catch (...) {
            Log("SetSite failed: %08X", winrt::to_hresult().value);
            SignalReady();
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void **site) noexcept override {
        *site = nullptr;
        return g_diagnostics ? g_diagnostics->QueryInterface(riid, site) : E_FAIL;
    }
};

class Factory : public winrt::implements<Factory, IClassFactory, winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID riid, void **object) noexcept override {
        try {
            *object = nullptr;
            if (outer) return CLASS_E_NOAGGREGATION;
            return winrt::make_self<Tap>().as<IObjectWithSite>()->QueryInterface(riid, object);
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};

// Two registry watches served by one thread that only ever sleeps in a wait. Thread
// pool waits were tried first, but their registrations were silently lost after a
// while, so settings changes stopped arriving.
struct RegistryWatch {
    HKEY key = nullptr;
    HANDLE event = nullptr;
    DWORD filter = 0;
    void (*onChange)() = nullptr;
};
RegistryWatch g_trayWatch;
RegistryWatch g_settingsWatch;

bool ArmWatch(RegistryWatch &watch) {
    if (!watch.key || !watch.event) return false;
    LSTATUS status = RegNotifyChangeKeyValue(watch.key, FALSE, watch.filter, watch.event, TRUE);
    if (status != ERROR_SUCCESS) Log("registry watch failed: %ld", status);
    return status == ERROR_SUCCESS;
}

DWORD WINAPI WatchThread(void *) {
    RegistryWatch *watches[] = {&g_settingsWatch, &g_trayWatch};
    HANDLE events[2];
    DWORD count = 0;
    RegistryWatch *armed[2];
    for (auto *watch : watches) {
        if (ArmWatch(*watch)) {
            armed[count] = watch;
            events[count++] = watch->event;
        }
    }
    if (!count) return 0;
    for (;;) {
        DWORD result = WaitForMultipleObjects(count, events, FALSE, INFINITE);
        if (result >= WAIT_OBJECT_0 + count) {
            Log("registry watch thread stopped: %lu", GetLastError());
            return 0;
        }
        auto &watch = *armed[result - WAIT_OBJECT_0];
        // Rearm first, on this same thread, so a change during the work isn't missed.
        ArmWatch(watch);
        try {
            watch.onChange();
        } catch (...) {
            Log("registry change handler failed");
        }
    }
}

// A new app's icon starts out not promoted. With the overflow arrow turned off it
// could never be reached, so with "show all" on, show it, unless the user hid that
// app. A new app means a new subkey, so only subkey creation is watched: icon data
// values change all the time and would wake this up for nothing.
void ApplyToNewEntries() {
    auto const hidden = LoadHidden();
    bool const showAll = g_showAllIcons;
    wchar_t name[256];
    for (DWORD i = 0;; i++) {
        DWORD length = ARRAYSIZE(name);
        if (RegEnumKeyExW(g_trayWatch.key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        DWORD value = 0, size = sizeof(value);
        if (RegGetValueW(g_trayWatch.key, name, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) continue;
        std::wstring path = ReadString(g_trayWatch.key, name, L"ExecutablePath");
        if (path.empty()) continue;
        std::wstring key = EntryKey(path, ReadString(g_trayWatch.key, name, L"IconGuid"));
        bool hide = hidden.count(key) > 0;
        if (!hide && !showAll) continue;
        DWORD wanted = hide ? 0 : 1;
        LSTATUS status = RegSetKeyValueW(g_trayWatch.key, name, L"IsPromoted", REG_DWORD, &wanted, sizeof(wanted));
        Log("new tray entry %ls: %s (%ld)", key.c_str(), wanted ? "shown" : "hidden", status);
    }
}

void OnTrayEntriesChanged() {
    // Explorer creates the subkey first and fills in its values right after.
    Sleep(1000);
    ApplyToNewEntries();
}

void LoadSettings() {
    g_hideVolume = ReadSetting(kHideVolume);
    g_hideMicrophone = ReadSetting(kHideMicrophone);
    g_hideLanguage = ReadSetting(kHideLanguage);
    g_showAllIcons = ReadSetting(kShowAllIcons);
    g_taskbarGlass = ReadSetting(kTaskbarGlass);
    g_menuGlass = ReadSetting(kMenuGlass);
    g_glassColor = ReadSettingDword(kGlassColor, kDefaultGlassColor) & 0xFFFFFF;
    g_glassOpacity = ReadSettingDword(kGlassOpacity, kDefaultGlassOpacity);
}

void RescanAll() {
    try {
        if (auto root = g_roots.controlCenter.get()) ScanGuarded(root, &ScanControlCenter);
        if (auto root = g_roots.mainStack.get()) ScanGuarded(root, &ScanMainStack);
        if (auto root = g_roots.languageStack.get()) ScanGuarded(root, &ScanLanguageStack);
    } catch (...) {
        Log("rescan failed");
    }
}

// The panel changed something. Scans touch XAML, so they run on the taskbar's thread.
void OnSettingsChanged() {
    LoadSettings();
    Log("settings: volume=%d microphone=%d language=%d showAll=%d", (int)g_hideVolume, (int)g_hideMicrophone, (int)g_hideLanguage, (int)g_showAllIcons);
    if (g_uiQueue) {
        g_uiQueue->queue.TryEnqueue([] {
            RescanAll();
            try {
                if (g_taskbarGlass || g_menuGlass) InstallGlassHooks();
                ApplyGlass();
            } catch (...) {
                Log("glass update failed");
            }
        });
    }
}

void StartWatches() {
    LoadSettings();

    g_settingsWatch.filter = REG_NOTIFY_CHANGE_LAST_SET;
    g_settingsWatch.onChange = &OnSettingsChanged;
    g_settingsWatch.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRoot, 0, nullptr, 0, KEY_READ | KEY_NOTIFY, nullptr, &g_settingsWatch.key, nullptr);
    if (status != ERROR_SUCCESS || !g_settingsWatch.event) Log("settings watch not started: %ld", status);

    g_trayWatch.filter = REG_NOTIFY_CHANGE_NAME;
    g_trayWatch.onChange = &OnTrayEntriesChanged;
    g_trayWatch.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    status = RegOpenKeyExW(HKEY_CURRENT_USER, kTrayRoot, 0, KEY_READ | KEY_SET_VALUE | KEY_NOTIFY, &g_trayWatch.key);
    if (status == ERROR_SUCCESS && g_trayWatch.event) {
        try {
            ApplyToNewEntries();
        } catch (...) {
            Log("ApplyToNewEntries failed");
        }
    } else {
        g_trayWatch.key = nullptr;
        Log("tray watch not started: %ld", status);
    }
    HANDLE thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        Log("watch thread not started: %lu", GetLastError());
    }
}

// Running TrayTweak.exe again puts everything back, e.g. after another tool reset the taskbar.
void ReapplyAll() {
    LoadSettings();
    RescanAll();
    try {
        if (g_taskbarGlass || g_menuGlass) InstallGlassHooks();
        ApplyGlass();
    } catch (...) {
        Log("glass update failed");
    }
    Log("reapplied");
}

DWORD WINAPI BootstrapThread(void *) {
    StartWatches();
    wchar_t location[MAX_PATH];
    DWORD len = GetModuleFileNameW(g_module, location, MAX_PATH);
    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto initialize = xaml ? reinterpret_cast<decltype(&InitializeXamlDiagnosticsEx)>(
                                 GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"))
                           : nullptr;
    HRESULT hr = E_FAIL;
    if (initialize && len && len < MAX_PATH) {
        // There is no way to ask which connection name is free.
        for (int i = 1; i <= 10000; i++) {
            wchar_t connection[64];
            swprintf_s(connection, L"VisualDiagConnection%d", i);
            hr = initialize(connection, GetCurrentProcessId(), L"", location, kClsidTap, nullptr);
            if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) break;
        }
    }
    Log("InitializeXamlDiagnosticsEx: %08X", hr);
    if (FAILED(hr)) SignalReady();
    return 0;
}

}  // namespace

extern "C" LRESULT CALLBACK HookProc(int code, WPARAM wParam, LPARAM lParam) {
    static const UINT message = RegisterWindowMessageW(L"TrayTweak.Start");
    if (code == HC_ACTION && reinterpret_cast<CWPSTRUCT *>(lParam)->message == message) {
        if (g_started.exchange(true)) {
            // Already running in this Explorer. This is the taskbar's thread, so XAML can be touched.
            if (GetCurrentThreadId() == g_uiThread) ReapplyAll();
            SignalReady();
        } else {
            // Stay loaded after the loader exits and removes its hook.
            HMODULE self;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&HookProc), &self);
            g_uiThread = GetCurrentThreadId();
            g_foundControlCenter = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_foundMainStack = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_foundLanguageStack = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_foundBackground = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE thread = CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *object) {
    try {
        *object = nullptr;
        if (clsid != kClsidTap) return CLASS_E_CLASSNOTAVAILABLE;
        return winrt::make_self<Factory>().as<IClassFactory>()->QueryInterface(riid, object);
    } catch (...) {
        return winrt::to_hresult();
    }
}

STDAPI DllCanUnloadNow() { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
