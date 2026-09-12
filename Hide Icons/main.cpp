#define OEMRESOURCE // Required for OCR_ system cursor constants
#include "resource.h"
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <sstream>
#include <Commctrl.h>
#include <codecvt>

// COM / Shell headers for robust DefView discovery
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <ole2.h>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

struct HotkeyConfig {
    UINT hotkey;
    UINT modifier;
};

struct AutoHideConfig {
    UINT  inactivitySeconds;
    UINT  minMouseMovement;
    bool  enableAutoHide;   // master on/off for the auto-hide feature
    bool  hideCursor;       // whether to also hide the cursor in auto-hide mode
};

// Global variables
HINSTANCE g_hInstance;
NOTIFYICONDATA g_nid;
HMENU g_hMenu;
bool g_isStartup = false;
HHOOK g_hKeyboardHook = nullptr;
HHOOK g_hMouseHook = nullptr;
HotkeyConfig g_hotkey = { 0, 0 };
AutoHideConfig g_autoHide = { 0, 25, true, true };
std::wstring customIconPath;

// Auto-hide state
#define TIMER_ID_AUTOHIDE   1
#define TIMER_INTERVAL_MS   500

bool g_iconsHidden = false;

// When true, the icons were hidden by the hotkey – only the hotkey can show
// them again.  Auto-hide logic is completely suppressed in this state.
bool g_manualHide = false;

// Strictly controlled wake flags to avoid the GetLastInputInfo race condition
bool g_pendingWake = false;
static LONG g_mouseAccumX = 0;
static LONG g_mouseAccumY = 0;
static DWORD g_mouseAccumResetTick = 0;

const wchar_t* APP_NAME = L"Hide Icons";
const wchar_t* APP_VERSION = L"v1.9.4";

// Registry keys
const wchar_t* REG_PATH = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* SETTINGS_REG_PATH = L"SOFTWARE\\Hide Icons";
const wchar_t* ICON_VALUE_NAME = L"CustomIcon";
const wchar_t* HOTKEY_VALUE_NAME = L"Hotkey";
const wchar_t* HOTKEY_MODIFIER_VALUE_NAME = L"HotkeyModifier";
const wchar_t* AUTOHIDE_SECONDS_VALUE_NAME = L"AutoHideSeconds";
const wchar_t* AUTOHIDE_MINMOUSE_VALUE_NAME = L"AutoHideMinMouseMovement";
const wchar_t* AUTOHIDE_ENABLE_VALUE_NAME = L"AutoHideEnable";
const wchar_t* AUTOHIDE_HIDECURSOR_VALUE_NAME = L"AutoHideHideCursor";

HICON hBlackIcon = (HICON)LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON2), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
HICON hWhiteIcon = (HICON)LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON3), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);

UINT WM_TASKBARCREATED = RegisterWindowMessage(L"TaskbarCreated");

void ToggleDesktopIcons();
bool AreDesktopIconsCurrentlyVisible();

// ─────────────────────────────────────────────────────────────────────────────
// Desktop Window Discovery
// ─────────────────────────────────────────────────────────────────────────────
static HWND GetDefViewByCOM()
{
    HWND hwndDefView = nullptr;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = SUCCEEDED(hr);

    IShellWindows* psw = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&psw))) && psw) {
        VARIANT vEmpty; VariantInit(&vEmpty);
        long lhwnd = 0;
        IDispatch* pdisp = nullptr;
        if (SUCCEEDED(psw->FindWindowSW(&vEmpty, &vEmpty, SWC_DESKTOP, &lhwnd, SWFO_NEEDDISPATCH, &pdisp)) && pdisp) {
            IServiceProvider* psp = nullptr;
            if (SUCCEEDED(pdisp->QueryInterface(IID_PPV_ARGS(&psp))) && psp) {
                IShellBrowser* psb = nullptr;
                if (SUCCEEDED(psp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&psb))) && psb) {
                    IShellView* psv = nullptr;
                    if (SUCCEEDED(psb->QueryActiveShellView(&psv)) && psv) {
                        IOleWindow* pow = nullptr;
                        if (SUCCEEDED(psv->QueryInterface(IID_PPV_ARGS(&pow))) && pow) {
                            pow->GetWindow(&hwndDefView);
                            pow->Release();
                        }
                        psv->Release();
                    }
                    psb->Release();
                }
                psp->Release();
            }
            pdisp->Release();
        }
        psw->Release();
    }
    if (needUninit) CoUninitialize();
    return hwndDefView;
}

static HWND GetDefViewByScan()
{
    HWND defView = nullptr;
    if (HWND prog = FindWindowW(L"Progman", nullptr)) {
        defView = FindWindowExW(prog, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defView) return defView;
    }
    HWND worker = nullptr;
    while ((worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr) {
        defView = FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defView) return defView;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// App Settings Registry
// ─────────────────────────────────────────────────────────────────────────────
void SaveHotkey(const HotkeyConfig& config) {
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueEx(hKey, HOTKEY_VALUE_NAME, 0, REG_DWORD, (const BYTE*)&config.hotkey, sizeof(config.hotkey));
        RegSetValueEx(hKey, HOTKEY_MODIFIER_VALUE_NAME, 0, REG_DWORD, (const BYTE*)&config.modifier, sizeof(config.modifier));
        RegCloseKey(hKey);
    }
}

HotkeyConfig LoadHotkey() {
    HKEY hKey;
    HotkeyConfig config = { 0, 0 };
    if (RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwType = 0, dwSize = sizeof(config.hotkey);
        RegQueryValueEx(hKey, HOTKEY_VALUE_NAME, 0, &dwType, (LPBYTE)&config.hotkey, &dwSize);
        dwSize = sizeof(config.modifier);
        RegQueryValueEx(hKey, HOTKEY_MODIFIER_VALUE_NAME, 0, &dwType, (LPBYTE)&config.modifier, &dwSize);
        RegCloseKey(hKey);
    }
    return config;
}

void SaveAutoHideConfig(const AutoHideConfig& config) {
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueEx(hKey, AUTOHIDE_SECONDS_VALUE_NAME, 0, REG_DWORD, (const BYTE*)&config.inactivitySeconds, sizeof(config.inactivitySeconds));
        RegSetValueEx(hKey, AUTOHIDE_MINMOUSE_VALUE_NAME, 0, REG_DWORD, (const BYTE*)&config.minMouseMovement, sizeof(config.minMouseMovement));
        DWORD enable = config.enableAutoHide ? 1 : 0;
        RegSetValueEx(hKey, AUTOHIDE_ENABLE_VALUE_NAME, 0, REG_DWORD, (const BYTE*)&enable, sizeof(enable));
        DWORD hideCursor = config.hideCursor ? 1 : 0;
        RegSetValueEx(hKey, AUTOHIDE_HIDECURSOR_VALUE_NAME, 0, REG_DWORD, (const BYTE*)&hideCursor, sizeof(hideCursor));
        RegCloseKey(hKey);
    }
}

AutoHideConfig LoadAutoHideConfig() {
    HKEY hKey;
    AutoHideConfig config = { 0, 25, true, true };
    if (RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwType = 0, dwSize = sizeof(DWORD), val = 0;

        RegQueryValueEx(hKey, AUTOHIDE_SECONDS_VALUE_NAME, 0, &dwType, (LPBYTE)&config.inactivitySeconds, &dwSize);

        dwSize = sizeof(DWORD);
        RegQueryValueEx(hKey, AUTOHIDE_MINMOUSE_VALUE_NAME, 0, &dwType, (LPBYTE)&config.minMouseMovement, &dwSize);

        dwSize = sizeof(DWORD); val = 1;
        if (RegQueryValueEx(hKey, AUTOHIDE_ENABLE_VALUE_NAME, 0, &dwType, (LPBYTE)&val, &dwSize) == ERROR_SUCCESS)
            config.enableAutoHide = (val != 0);

        dwSize = sizeof(DWORD); val = 1;
        if (RegQueryValueEx(hKey, AUTOHIDE_HIDECURSOR_VALUE_NAME, 0, &dwType, (LPBYTE)&val, &dwSize) == ERROR_SUCCESS)
            config.hideCursor = (val != 0);

        RegCloseKey(hKey);
    }
    return config;
}

// ─────────────────────────────────────────────────────────────────────────────
// Dialogs
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK AutoHideDialogProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        CheckDlgButton(hWnd, IDC_AUTOHIDE_ENABLE_CHECK, g_autoHide.enableAutoHide ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hWnd, IDC_AUTOHIDE_HIDECURSOR_CHECK, g_autoHide.hideCursor ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(hWnd, IDC_AUTOHIDE_SECONDS_EDIT, g_autoHide.inactivitySeconds, FALSE);
        SetDlgItemInt(hWnd, IDC_AUTOHIDE_MINMOUSE_EDIT, g_autoHide.minMouseMovement, FALSE);

        POINT cursorPos;
        GetCursorPos(&cursorPos);
        cursorPos.x += 20;
        cursorPos.y += 20;

        RECT screenRect;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &screenRect, 0);
        RECT dialogRect;
        GetWindowRect(hWnd, &dialogRect);
        int dialogWidth = dialogRect.right - dialogRect.left;
        int dialogHeight = dialogRect.bottom - dialogRect.top;

        if (cursorPos.x + dialogWidth > screenRect.right)  cursorPos.x = screenRect.right - dialogWidth - 10;
        if (cursorPos.y + dialogHeight > screenRect.bottom) cursorPos.y = screenRect.bottom - dialogHeight - 10;

        SetWindowPos(hWnd, HWND_TOP, cursorPos.x, cursorPos.y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            BOOL translated = FALSE;
            UINT seconds = GetDlgItemInt(hWnd, IDC_AUTOHIDE_SECONDS_EDIT, &translated, FALSE);
            if (!translated) seconds = 0;

            UINT minMouse = GetDlgItemInt(hWnd, IDC_AUTOHIDE_MINMOUSE_EDIT, &translated, FALSE);
            if (!translated || minMouse < 1) minMouse = 1;

            g_autoHide.inactivitySeconds = seconds;
            g_autoHide.minMouseMovement = minMouse;
            g_autoHide.enableAutoHide = (IsDlgButtonChecked(hWnd, IDC_AUTOHIDE_ENABLE_CHECK) == BST_CHECKED);
            g_autoHide.hideCursor = (IsDlgButtonChecked(hWnd, IDC_AUTOHIDE_HIDECURSOR_CHECK) == BST_CHECKED);
            SaveAutoHideConfig(g_autoHide);
            EndDialog(hWnd, IDOK);
            break;
        }
        case IDCANCEL:
            EndDialog(hWnd, IDCANCEL);
            break;
        }
        break;

    default:
        return FALSE;
    }
    return TRUE;
}

void SetAutoHide(HWND hWnd) {
    DialogBox(g_hInstance, MAKEINTRESOURCE(IDD_AUTOHIDE_DIALOG), hWnd, AutoHideDialogProc);
}

LRESULT CALLBACK HotkeyDialogProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        SendDlgItemMessage(hWnd, IDC_HOTKEY_EDIT, HKM_SETHOTKEY, MAKEWORD(g_hotkey.hotkey, g_hotkey.modifier), 0);

        POINT cursorPos;
        GetCursorPos(&cursorPos);
        cursorPos.x += 20;
        cursorPos.y += 20;

        RECT screenRect;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &screenRect, 0);
        RECT dialogRect;
        GetWindowRect(hWnd, &dialogRect);
        int dialogWidth = dialogRect.right - dialogRect.left;
        int dialogHeight = dialogRect.bottom - dialogRect.top;

        if (cursorPos.x + dialogWidth > screenRect.right)  cursorPos.x = screenRect.right - dialogWidth - 10;
        if (cursorPos.y + dialogHeight > screenRect.bottom) cursorPos.y = screenRect.bottom - dialogHeight - 10;

        SetWindowPos(hWnd, HWND_TOP, cursorPos.x, cursorPos.y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            WORD hotkey = (WORD)SendDlgItemMessage(hWnd, IDC_HOTKEY_EDIT, HKM_GETHOTKEY, 0, 0);
            UINT vk = LOBYTE(hotkey);
            UINT mod = HIBYTE(hotkey);

            UINT modifier = 0;
            if (mod & HOTKEYF_CONTROL) modifier |= MOD_CONTROL;
            if (mod & HOTKEYF_SHIFT)   modifier |= MOD_SHIFT;
            if (mod & HOTKEYF_ALT)     modifier |= MOD_ALT;

            g_hotkey.hotkey = vk;
            g_hotkey.modifier = modifier;
            EndDialog(hWnd, IDOK);
            break;
        }
        case IDCANCEL:
            EndDialog(hWnd, IDCANCEL);
            break;
        case IDC_CLEAR_BUTTON:
            SendDlgItemMessage(hWnd, IDC_HOTKEY_EDIT, HKM_SETHOTKEY, 0, 0);
            break;
        }
        break;

    default:
        return FALSE;
    }
    return TRUE;
}

void SetHotkey(HWND hWnd) {
    if (!FindWindow(nullptr, L"Hotkey Option")) {
        if (DialogBox(g_hInstance, MAKEINTRESOURCE(IDD_HOTKEY_DIALOG), hWnd, HotkeyDialogProc) == IDOK) {
            SaveHotkey(g_hotkey);
        }
    }
}

void RemoveHotkey(HWND hWnd) {
    g_hotkey.hotkey = 0;
    g_hotkey.modifier = 0;
    SaveHotkey(g_hotkey);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tray & Startup
// ─────────────────────────────────────────────────────────────────────────────
bool IsTaskbarDarkMode() {
    HKEY hKey;
    DWORD value = 0, size = sizeof(DWORD);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueEx(hKey, L"SystemUsesLightTheme", nullptr, nullptr, (LPBYTE)&value, &size);
        RegCloseKey(hKey);
        return value == 0;
    }
    return false;
}

bool IsStartupEnabled() {
    HKEY key;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH];
        DWORD bufferSize = sizeof(value);
        bool found = (RegQueryValueEx(key, APP_NAME, nullptr, nullptr, (LPBYTE)value, &bufferSize) == ERROR_SUCCESS);
        RegCloseKey(key);
        return found;
    }
    return false;
}

void ToggleStartup() {
    HKEY key;
    if (g_isStartup) {
        if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValue(key, APP_NAME);
            RegCloseKey(key);
        }
    }
    else {
        wchar_t exePath[MAX_PATH];
        GetModuleFileName(nullptr, exePath, MAX_PATH);
        if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegSetValueEx(key, APP_NAME, 0, REG_SZ, (const BYTE*)exePath, (DWORD)(wcslen(exePath) + 1) * sizeof(wchar_t));
            RegCloseKey(key);
        }
    }
    g_isStartup = !g_isStartup;
}

void SaveCustomIconPath(const wchar_t* iconPath) {
    HKEY key;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueEx(key, ICON_VALUE_NAME, 0, REG_SZ, (const BYTE*)iconPath, (DWORD)(wcslen(iconPath) + 1) * sizeof(wchar_t));
        RegCloseKey(key);
    }
}

bool LoadCustomIconPath(std::wstring& iconPath) {
    HKEY key;
    wchar_t buffer[MAX_PATH];
    DWORD bufferSize = sizeof(buffer);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegQueryValueEx(key, ICON_VALUE_NAME, nullptr, nullptr, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS) {
            iconPath = buffer;
            RegCloseKey(key);
            return true;
        }
        RegCloseKey(key);
    }
    return false;
}

void ChangeTrayIcon() {
    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAME ofn = {};
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.lpstrFilter = L"Icon Files\0*.ico\0All Files\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Select a New Tray Icon";

    if (GetOpenFileName(&ofn)) {
        HICON hNewIcon = (HICON)LoadImage(nullptr, filePath, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (hNewIcon) {
            g_nid.hIcon = hNewIcon;
            Shell_NotifyIcon(NIM_MODIFY, &g_nid);
            SaveCustomIconPath(filePath);
        }
    }
}

void UpdateIconColor() {
    LoadCustomIconPath(customIconPath);
    if (!customIconPath.empty()) return;
    g_nid.hIcon = IsTaskbarDarkMode() ? hWhiteIcon : hBlackIcon;
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

void ResetTrayIcon() {
    g_nid.hIcon = IsTaskbarDarkMode() ? hWhiteIcon : hBlackIcon;
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
    SaveCustomIconPath(L"");
}

void CreateTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP + 1;
    g_nid.hIcon = IsTaskbarDarkMode() ? hWhiteIcon : hBlackIcon;
    wcscpy_s(g_nid.szTip, (L"Hide Icons " + std::wstring(APP_VERSION)).c_str());

    if (LoadCustomIconPath(customIconPath) && !customIconPath.empty()) {
        HICON hCustomIcon = (HICON)LoadImage(nullptr, customIconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (hCustomIcon) g_nid.hIcon = hCustomIcon;
    }

    Shell_NotifyIcon(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIcon(NIM_SETVERSION, &g_nid);
    g_isStartup = IsStartupEnabled();
}

void RemoveTrayIcon() {
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
}

// ─────────────────────────────────────────────────────────────────────────────
// True Stealth Visibility & Cursor Control
// ─────────────────────────────────────────────────────────────────────────────

HCURSOR g_savedCursors[13] = { nullptr };
const DWORD g_cursorIDs[13] = {
    OCR_NORMAL, OCR_IBEAM, OCR_WAIT, OCR_CROSS, OCR_UP, OCR_SIZENWSE,
    OCR_SIZENESW, OCR_SIZEWE, OCR_SIZENS, OCR_SIZEALL, OCR_NO, OCR_HAND,
    OCR_APPSTARTING
};

void SetCursorVisibility(bool show) {
    static bool isHidden = false;

    if (show && isHidden) {
        for (int i = 0; i < 13; ++i) {
            if (g_savedCursors[i]) {
                HCURSOR hCopy = (HCURSOR)CopyImage(g_savedCursors[i], IMAGE_CURSOR, 0, 0, 0);
                SetSystemCursor(hCopy, g_cursorIDs[i]);
            }
        }
        isHidden = false;
    }
    else if (!show && !isHidden) {
        for (int i = 0; i < 13; ++i) {
            if (!g_savedCursors[i]) {
                HANDLE hSysCursor = LoadImage(nullptr, MAKEINTRESOURCE(g_cursorIDs[i]), IMAGE_CURSOR, 0, 0, LR_SHARED);
                g_savedCursors[i] = (HCURSOR)CopyImage(hSysCursor, IMAGE_CURSOR, 0, 0, 0);
            }
        }

        BYTE ANDmaskCursor[128];
        memset(ANDmaskCursor, 0xFF, sizeof(ANDmaskCursor));
        BYTE XORmaskCursor[128];
        memset(XORmaskCursor, 0x00, sizeof(XORmaskCursor));

        for (DWORD id : g_cursorIDs) {
            HCURSOR hInvisibleCursor = CreateCursor(nullptr, 0, 0, 32, 32, ANDmaskCursor, XORmaskCursor);
            SetSystemCursor(hInvisibleCursor, id);
        }
        isHidden = true;
    }
}

void CleanupCursors() {
    for (int i = 0; i < 13; ++i) {
        if (g_savedCursors[i]) {
            DestroyCursor(g_savedCursors[i]);
            g_savedCursors[i] = nullptr;
        }
    }
}

void SetDesktopIconsVisibility(bool show) {
    HWND defView = GetDefViewByCOM();
    if (!defView) defView = GetDefViewByScan();
    if (!defView) return;

    HWND hListView = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
    if (hListView) {
        ShowWindow(hListView, show ? SW_SHOW : SW_HIDE);
    }
}

bool AreDesktopIconsCurrentlyVisible() {
    HWND defView = GetDefViewByCOM();
    if (!defView) defView = GetDefViewByScan();
    if (defView) {
        HWND hListView = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
        if (hListView) {
            return IsWindowVisible(hListView) != 0;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// State Transition Management
// ─────────────────────────────────────────────────────────────────────────────

void ResetWakeAccumulators() {
    g_pendingWake = false;
    g_mouseAccumX = 0;
    g_mouseAccumY = 0;
    g_mouseAccumResetTick = GetTickCount();
}

// Hide via auto-hide: hides icons and (if enabled) the cursor.
// Does NOT set g_manualHide.
void HideDesktopIcons() {
    if (!g_iconsHidden) {
        g_iconsHidden = true;
        SetDesktopIconsVisibility(false);
        if (g_autoHide.hideCursor) {
            SetCursorVisibility(false);
        }
        ResetWakeAccumulators();
    }
}

// Show after auto-hide: restores icons and cursor.
// Only called when g_manualHide is false.
void ShowDesktopIcons() {
    if (g_iconsHidden) {
        g_iconsHidden = false;
        SetDesktopIconsVisibility(true);
        SetCursorVisibility(true); // always safe – no-op if cursor wasn't hidden
        ResetWakeAccumulators();
    }
}

// Hide via hotkey: only hides icons, never the cursor.
// Sets g_manualHide so auto-hide logic is suppressed.
void ManualHideDesktopIcons() {
    g_manualHide = true;
    g_iconsHidden = true;
    SetDesktopIconsVisibility(false);
    // Cursor is intentionally left visible for manual hide.
    ResetWakeAccumulators();
}

// Show after hotkey: restores icons, clears manual-hide flag.
void ManualShowDesktopIcons() {
    g_manualHide = false;
    g_iconsHidden = false;
    SetDesktopIconsVisibility(true);
    SetCursorVisibility(true); // no-op if cursor wasn't hidden; safe to call
    ResetWakeAccumulators();
}

// Called by tray left-click – behaves like the hotkey toggle.
void ToggleDesktopIcons() {
    if (g_iconsHidden) ManualShowDesktopIcons();
    else               ManualHideDesktopIcons();
}

// ─────────────────────────────────────────────────────────────────────────────
// Hooks & Timers
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_MOUSEMOVE) {
            static LONG lastX = LONG_MIN, lastY = LONG_MIN;
            MSLLHOOKSTRUCT* p = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

            if (lastX != LONG_MIN) {
                LONG dx = p->pt.x - lastX;
                LONG dy = p->pt.y - lastY;

                // Only accumulate if hidden AND not in manual-hide mode
                if (g_iconsHidden && !g_manualHide) {
                    DWORD now = GetTickCount();
                    if (now - g_mouseAccumResetTick >= 1000) {
                        g_mouseAccumX = 0;
                        g_mouseAccumY = 0;
                        g_mouseAccumResetTick = now;
                    }
                    g_mouseAccumX += (dx < 0 ? -dx : dx);
                    g_mouseAccumY += (dy < 0 ? -dy : dy);
                }
            }
            lastX = p->pt.x;
            lastY = p->pt.y;
        }
        else if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
            wParam == WM_MBUTTONDOWN || wParam == WM_MOUSEWHEEL) {
            // Clicks/scrolls wake the system only when auto-hidden (not manual-hidden)
            if (g_iconsHidden && !g_manualHide) g_pendingWake = true;
        }
    }
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pKeyInfo = (KBDLLHOOKSTRUCT*)lParam;
        bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

        if (isKeyDown) {
            bool isHotkey = false;

            if (g_hotkey.hotkey != 0 && pKeyInfo->vkCode == g_hotkey.hotkey) {
                bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                bool altPressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

                if ((g_hotkey.modifier & MOD_CONTROL) == (ctrlPressed ? MOD_CONTROL : 0) &&
                    (g_hotkey.modifier & MOD_SHIFT) == (shiftPressed ? MOD_SHIFT : 0) &&
                    (g_hotkey.modifier & MOD_ALT) == (altPressed ? MOD_ALT : 0))
                {
                    // Hotkey pressed: always toggle, regardless of manual/auto mode
                    if (g_manualHide || !g_iconsHidden) {
                        // Currently manually hidden, or visible → use manual-hide path
                        if (g_iconsHidden) ManualShowDesktopIcons();
                        else               ManualHideDesktopIcons();
                    }
                    else {
                        // Currently auto-hidden → hotkey cancels auto-hide and shows
                        ShowDesktopIcons();
                    }
                    isHotkey = true;
                }
            }

            // Any key that isn't the hotkey wakes the system only in auto-hide mode
            if (!isHotkey && g_iconsHidden && !g_manualHide) {
                g_pendingWake = true;
            }
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

void InstallKeyboardHook() {
    g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_hInstance, 0);
}

void InstallMouseHook() {
    g_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, g_hInstance, 0);
}

void UninstallKeyboardHook() {
    if (g_hKeyboardHook) { UnhookWindowsHookEx(g_hKeyboardHook); g_hKeyboardHook = nullptr; }
}

void UninstallMouseHook() {
    if (g_hMouseHook) { UnhookWindowsHookEx(g_hMouseHook); g_hMouseHook = nullptr; }
}

void OnAutoHideTimer() {
    // If auto-hide is disabled, or we're in manual-hide mode, do nothing.
    if (!g_autoHide.enableAutoHide) return;
    if (g_manualHide) return;

    if (!g_iconsHidden) {
        // inactivitySeconds == 0 means auto-hide is effectively off
        if (g_autoHide.inactivitySeconds == 0) return;

        LASTINPUTINFO lii;
        lii.cbSize = sizeof(LASTINPUTINFO);
        if (!GetLastInputInfo(&lii)) return;

        DWORD idleMs = GetTickCount() - lii.dwTime;
        if (idleMs >= g_autoHide.inactivitySeconds * 1000U) {
            HideDesktopIcons();
        }
    }
    else {
        LONG totalMovement = g_mouseAccumX + g_mouseAccumY;
        if (totalMovement >= (LONG)g_autoHide.minMouseMovement || g_pendingWake) {
            ShowDesktopIcons();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Window procedure
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TASKBARCREATED) {
        CreateTrayIcon(hWnd);
        return 0;
    }

    switch (message) {
    case WM_TIMER:
        if (wParam == TIMER_ID_AUTOHIDE) {
            OnAutoHideTimer();
        }
        break;

    case WM_APP + 1:
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            CheckMenuItem(g_hMenu, 1, MF_BYCOMMAND | (g_isStartup ? MF_CHECKED : MF_UNCHECKED));
            TrackPopupMenu(g_hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
        }
        else if (LOWORD(lParam) == WM_LBUTTONUP) {
            ToggleDesktopIcons();
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1:  ToggleStartup();     break;
        case 2:  ChangeTrayIcon();    break;
        case 3:  ResetTrayIcon();     break;
        case 4:  SetHotkey(hWnd);     break;
        case 5:  RemoveHotkey(hWnd);  break;
        case 6:  SetAutoHide(hWnd);   break;
        case 7:
            MessageBox(hWnd, (L"Hide Icons " + std::wstring(APP_VERSION) + L"\nCreated by emp0ry").c_str(), L"About", MB_OK | MB_ICONINFORMATION);
            break;
        case 8:
            SendMessage(hWnd, WM_CLOSE, 0, 0);
            break;
        }
        break;

    case WM_SETTINGCHANGE:
        UpdateIconColor();
        break;

    case WM_DESTROY:
        ShowDesktopIcons();
        KillTimer(hWnd, TIMER_ID_AUTOHIDE);
        RemoveTrayIcon();
        CleanupCursors();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    CreateMutexA(0, FALSE, "Local\\HideIcons_TrayApp");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBox(NULL, L"Hide Icons is already running!", NULL, MB_ICONERROR | MB_OK);
        return -1;
    }

    g_hInstance = hInstance;

    // Sync logical state with the live window visibility
    g_iconsHidden = !AreDesktopIconsCurrentlyVisible();
    // If icons are already hidden at startup we can't know whether it was manual
    // or auto – treat it as manual so the hotkey is required to restore them.
    if (g_iconsHidden) g_manualHide = true;

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"HideIconsTrayApp";
    RegisterClass(&wc);

    HWND hWnd = CreateWindowEx(0, L"HideIconsTrayApp", L"Hide Icons", 0, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return -1;

    g_hotkey = LoadHotkey();
    g_autoHide = LoadAutoHideConfig();

    ResetWakeAccumulators();

    InstallKeyboardHook();
    InstallMouseHook();
    CreateTrayIcon(hWnd);

    SetTimer(hWnd, TIMER_ID_AUTOHIDE, TIMER_INTERVAL_MS, nullptr);

    g_hMenu = CreatePopupMenu();
    AppendMenu(g_hMenu, MF_STRING, 1, L"Run at Startup");
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, 2, L"Change Icon");
    AppendMenu(g_hMenu, MF_STRING, 3, L"Reset Icon");
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, 4, L"Set Hotkey");
    AppendMenu(g_hMenu, MF_STRING, 5, L"Remove Hotkey");
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, 6, L"Auto-Hide Settings");
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, 7, L"About");
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, 8, L"Exit");

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DestroyMenu(g_hMenu);
    UninstallKeyboardHook();
    UninstallMouseHook();
    return 0;
}