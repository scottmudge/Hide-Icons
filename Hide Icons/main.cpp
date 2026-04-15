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
    UINT hotkey;          // Virtual key code for the hotkey (e.g., VK_F1, VK_A, etc.)
    UINT modifier;        // Modifier key (e.g., MOD_SHIFT, MOD_CONTROL, MOD_ALT)
};

struct AutoHideConfig {
    UINT inactivitySeconds;   // Seconds of inactivity before hiding icons (0 = disabled)
    UINT minMouseMovement;    // Minimum pixel movement (accumulated over 1s) to un-hide icons
};

// Global variables
HINSTANCE g_hInstance;
NOTIFYICONDATA g_nid;
HMENU g_hMenu;
bool g_isStartup = false;
HHOOK g_hKeyboardHook = nullptr;
HHOOK g_hMouseHook = nullptr;
HotkeyConfig g_hotkey = { 0, 0 };
AutoHideConfig g_autoHide = { 0, 25 }; // Default: disabled, 25px threshold
std::wstring customIconPath;

// Auto-hide state
#define TIMER_ID_AUTOHIDE   1
#define TIMER_INTERVAL_MS   500   // Poll every 500ms

// Icons are considered hidden when this is true (tracks the *logical* state we set,
// since the registry HideIcons value is toggled by SendMessage and we can't read it
// synchronously after the toggle).
bool g_iconsHidden = false;

// Mouse movement accumulator — reset every 1 second by the polling timer
static LONG g_mouseAccumX = 0;
static LONG g_mouseAccumY = 0;
static DWORD g_mouseAccumResetTick = 0; // tick of the last 1-second reset

const wchar_t* APP_NAME = L"Hide Icons";
const wchar_t* APP_VERSION = L"v1.9.0";

// Registry keys
const wchar_t* REG_PATH = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* SETTINGS_REG_PATH = L"SOFTWARE\\Hide Icons";
const wchar_t* ICON_VALUE_NAME = L"CustomIcon";
const wchar_t* DESKTOP_ICON_STATE = L"DesktopIconsState";
const wchar_t* HOTKEY_VALUE_NAME = L"Hotkey";
const wchar_t* HOTKEY_MODIFIER_VALUE_NAME = L"HotkeyModifier";
const wchar_t* AUTOHIDE_SECONDS_VALUE_NAME = L"AutoHideSeconds";
const wchar_t* AUTOHIDE_MINMOUSE_VALUE_NAME = L"AutoHideMinMouseMovement";

HICON hBlackIcon = (HICON)LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON2), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
HICON hWhiteIcon = (HICON)LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_ICON3), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);

// Define WM_TASKBARCREATED
UINT WM_TASKBARCREATED = RegisterWindowMessage(L"TaskbarCreated");

// Forward declarations
void ToggleDesktopIcons();
bool AreDesktopIconsCurrentlyVisible();

// ─────────────────────────────────────────────────────────────────────────────
// COM-based way to get the real SHELLDLL_DefView window
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

// Fallback scanner to find SHELLDLL_DefView if COM path fails
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

// Helper to read actual user HideIcons value for state reflection
static bool ReadHideIconsReg(bool& visibleOut)
{
    DWORD val = 0, type = 0, cb = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"HideIcons", RRF_RT_REG_DWORD, &type, &val, &cb) == ERROR_SUCCESS)
    {
        visibleOut = (val == 0);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Registry: Hotkey
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

// ─────────────────────────────────────────────────────────────────────────────
// Registry: AutoHide settings
// ─────────────────────────────────────────────────────────────────────────────
void SaveAutoHideConfig(const AutoHideConfig& config) {
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueEx(hKey, AUTOHIDE_SECONDS_VALUE_NAME, 0, REG_DWORD, (const BYTE*)&config.inactivitySeconds, sizeof(config.inactivitySeconds));
        RegSetValueEx(hKey, AUTOHIDE_MINMOUSE_VALUE_NAME, 0, REG_DWORD, (const BYTE*)&config.minMouseMovement, sizeof(config.minMouseMovement));
        RegCloseKey(hKey);
    }
}

AutoHideConfig LoadAutoHideConfig() {
    HKEY hKey;
    AutoHideConfig config = { 0, 25 }; // defaults
    if (RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwType = 0, dwSize = sizeof(DWORD);
        RegQueryValueEx(hKey, AUTOHIDE_SECONDS_VALUE_NAME, 0, &dwType, (LPBYTE)&config.inactivitySeconds, &dwSize);
        dwSize = sizeof(DWORD);
        RegQueryValueEx(hKey, AUTOHIDE_MINMOUSE_VALUE_NAME, 0, &dwType, (LPBYTE)&config.minMouseMovement, &dwSize);
        RegCloseKey(hKey);
    }
    return config;
}

// ─────────────────────────────────────────────────────────────────────────────
// AutoHide settings dialog
// ─────────────────────────────────────────────────────────────────────────────

// Dialog resource IDs — add these to your .rc file:
//   IDD_AUTOHIDE_DIALOG   a dialog with:
//     IDC_AUTOHIDE_SECONDS_EDIT   EDITTEXT  (number, 0 = disable)
//     IDC_AUTOHIDE_MINMOUSE_EDIT  EDITTEXT  (number in pixels)
//     IDOK / IDCANCEL             PUSHBUTTONs
//
// If you prefer, you can build the dialog in code (see the WM_INITDIALOG note
// in the proc below) — but a resource dialog is cleaner.

LRESULT CALLBACK AutoHideDialogProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        // Populate fields with current values
        SetDlgItemInt(hWnd, IDC_AUTOHIDE_SECONDS_EDIT, g_autoHide.inactivitySeconds, FALSE);
        SetDlgItemInt(hWnd, IDC_AUTOHIDE_MINMOUSE_EDIT, g_autoHide.minMouseMovement, FALSE);

        // Position dialog near the cursor
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
        if (cursorPos.y + dialogHeight > screenRect.bottom)  cursorPos.y = screenRect.bottom - dialogHeight - 10;

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
            if (!translated || minMouse < 1) minMouse = 1; // at least 1 px

            g_autoHide.inactivitySeconds = seconds;
            g_autoHide.minMouseMovement = minMouse;
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

// ─────────────────────────────────────────────────────────────────────────────
// Hotkey dialog (unchanged from original)
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK HotkeyDialogProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        SendDlgItemMessage(hWnd, IDC_HOTKEY_EDIT, HKM_SETHOTKEY,
            MAKEWORD(g_hotkey.hotkey, g_hotkey.modifier), 0);

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
        if (cursorPos.y + dialogHeight > screenRect.bottom)  cursorPos.y = screenRect.bottom - dialogHeight - 10;

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
// Theme / startup helpers (unchanged)
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

// ─────────────────────────────────────────────────────────────────────────────
// Custom icon helpers (unchanged)
// ─────────────────────────────────────────────────────────────────────────────
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

bool GetDesktopIconsRegistryState() {
    HKEY key;
    BYTE state;
    DWORD stateSize = sizeof(state);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_PATH, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegQueryValueEx(key, DESKTOP_ICON_STATE, nullptr, nullptr, &state, &stateSize) == ERROR_SUCCESS) {
            RegCloseKey(key);
            return state != 0;
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
        else {
            MessageBox(nullptr, L"Failed to load the icon.", L"Error", MB_ICONERROR);
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
// Desktop icon visibility helpers
// ─────────────────────────────────────────────────────────────────────────────

// Read the actual Explorer registry state: true = visible, false = hidden
bool AreDesktopIconsCurrentlyVisible() {
    bool visible = true;
    ReadHideIconsReg(visible);
    return visible;
}

// Toggle icons and keep g_iconsHidden in sync
void ToggleDesktopIcons() {
    HWND defView = GetDefViewByCOM();
    if (!defView) defView = GetDefViewByScan();
    if (!defView) return;

    PostMessage(defView, WM_COMMAND, 29698, 0);

    // Flip our logical state
    g_iconsHidden = !g_iconsHidden;
}

// Hide icons (only if currently visible)
void HideDesktopIcons() {
    if (!g_iconsHidden) {
        ToggleDesktopIcons();
    }
}

// Show icons (only if currently hidden)
void ShowDesktopIcons() {
    if (g_iconsHidden) {
        ToggleDesktopIcons();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Low-level mouse hook — PASSIVE accumulator only
//
// This hook never calls anything other than CallNextHookEx, so it has
// zero effect on what other applications / games receive.
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE) {
        // lParam is a pointer to MSLLHOOKSTRUCT; we only read the delta
        // fields using RAWMOUSE-style deltas by comparing successive positions.
        // However MSLLHOOKSTRUCT gives screen coordinates, not deltas.
        // We compute deltas ourselves from consecutive positions.
        static LONG lastX = LONG_MIN, lastY = LONG_MIN;
        MSLLHOOKSTRUCT* p = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        if (lastX != LONG_MIN) {
            LONG dx = p->pt.x - lastX;
            LONG dy = p->pt.y - lastY;

            // Reset accumulator every 1 second
            DWORD now = GetTickCount();
            if (now - g_mouseAccumResetTick >= 1000) {
                g_mouseAccumX = 0;
                g_mouseAccumY = 0;
                g_mouseAccumResetTick = now;
            }

            // Accumulate absolute deltas
            g_mouseAccumX += (dx < 0 ? -dx : dx);
            g_mouseAccumY += (dy < 0 ? -dy : dy);
        }
        lastX = p->pt.x;
        lastY = p->pt.y;
    }
    // Always pass to next hook — we never consume anything
    return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// Low-level keyboard hook (unchanged logic, kept passive)
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pKeyInfo = (KBDLLHOOKSTRUCT*)lParam;
        bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

        if (isKeyDown && g_hotkey.hotkey != 0 && pKeyInfo->vkCode == g_hotkey.hotkey) {
            bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            bool altPressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

            if ((g_hotkey.modifier & MOD_CONTROL) == (ctrlPressed ? MOD_CONTROL : 0) &&
                (g_hotkey.modifier & MOD_SHIFT) == (shiftPressed ? MOD_SHIFT : 0) &&
                (g_hotkey.modifier & MOD_ALT) == (altPressed ? MOD_ALT : 0))
            {
                ToggleDesktopIcons();
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

// ─────────────────────────────────────────────────────────────────────────────
// Auto-hide polling timer
//
// Called every TIMER_INTERVAL_MS (500ms).
//
// Logic:
//   • If auto-hide is enabled and icons are currently visible:
//       – Ask Windows how long since last keyboard/mouse input (GetLastInputInfo).
//       – If idle >= inactivitySeconds → hide icons.
//
//   • If icons are currently hidden:
//       – Check accumulated mouse movement over the last 1-second window.
//       – If movement >= minMouseMovement → show icons (reset accumulator).
//       – Any keyboard activity (GetLastInputInfo reset) also un-hides.
//
// GetLastInputInfo is a simple non-blocking query; it has NO effect on the
// input queue and is safe to call from any thread / at any frequency.
// ─────────────────────────────────────────────────────────────────────────────
void OnAutoHideTimer() {
    if (g_autoHide.inactivitySeconds == 0) return; // feature disabled

    LASTINPUTINFO lii;
    lii.cbSize = sizeof(LASTINPUTINFO);
    if (!GetLastInputInfo(&lii)) return;

    DWORD idleMs = GetTickCount() - lii.dwTime;

    if (!g_iconsHidden) {
        // --- Hide path ---
        if (idleMs >= g_autoHide.inactivitySeconds * 1000U) {
            HideDesktopIcons();
        }
    }
    else {
        // --- Show path ---
        // Un-hide on sufficient mouse movement
        LONG totalMovement = g_mouseAccumX + g_mouseAccumY;
        if (totalMovement >= (LONG)g_autoHide.minMouseMovement) {
            // Reset accumulator so we don't fire repeatedly
            g_mouseAccumX = 0;
            g_mouseAccumY = 0;
            g_mouseAccumResetTick = GetTickCount();
            ShowDesktopIcons();
            return;
        }

        // Also un-hide on any keyboard activity (idle time resets on key press)
        // We consider "recent activity" as idle < 500ms while icons are hidden.
        // This means a key press will un-hide within one timer tick (500ms).
        if (idleMs < 500) {
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
        case 6:  SetAutoHide(hWnd);   break;   // NEW
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
        KillTimer(hWnd, TIMER_ID_AUTOHIDE);
        RemoveTrayIcon();
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

    // Sync g_iconsHidden with the actual current Explorer state at startup
    {
        bool visible = true;
        ReadHideIconsReg(visible);
        g_iconsHidden = !visible;
    }

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"HideIconsTrayApp";
    RegisterClass(&wc);

    HWND hWnd = CreateWindowEx(0, L"HideIconsTrayApp", L"Hide Icons", 0, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return -1;

    g_hotkey = LoadHotkey();
    g_autoHide = LoadAutoHideConfig();

    // Initialise the mouse-movement accumulator timestamp
    g_mouseAccumResetTick = GetTickCount();

    InstallKeyboardHook();
    InstallMouseHook();
    CreateTrayIcon(hWnd);

    // Start the polling timer (fires every 500ms)
    SetTimer(hWnd, TIMER_ID_AUTOHIDE, TIMER_INTERVAL_MS, nullptr);

    // ── Context menu ──────────────────────────────────────────────────────────
    g_hMenu = CreatePopupMenu();
    AppendMenu(g_hMenu, MF_STRING, 1, L"Run at Startup");
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, 2, L"Change Icon");
    AppendMenu(g_hMenu, MF_STRING, 3, L"Reset Icon");
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, 4, L"Set Hotkey");
    AppendMenu(g_hMenu, MF_STRING, 5, L"Remove Hotkey");
    AppendMenu(g_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(g_hMenu, MF_STRING, 6, L"Auto-Hide Settings");   // NEW
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