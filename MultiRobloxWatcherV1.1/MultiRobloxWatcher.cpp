/*
 * MultiRobloxWatcher
 * -------------------
 * Tray-icon background watcher that lets multiple ROBLOX instances run.
 *
 * Newer ROBLOX builds no longer rely solely on a named Mutex
 * ("ROBLOX_singletonMutex") to prevent a second instance - they also use a
 * named Event object ("...\BaseNamedObjects\ROBLOX_singletonEvent"). Simply
 * opening-and-closing our own handle to that event does nothing, because the
 * object stays alive until EVERY handle to it (including the one Roblox
 * itself holds) is closed.
 *
 * So instead, this watcher:
 *   1. Every 2 seconds, cheaply lists every running RobloxPlayerBeta.exe
 *      (CreateToolhelp32Snapshot - negligible CPU cost).
 *   2. Every NEW Roblox PID it sees is tracked on its own. After ~6 seconds
 *      (counted per process), it enumerates that process's open handles
 *      (NtQuerySystemInformation), finds the Event object whose name ends in
 *      "ROBLOX_singletonEvent", and force-closes it inside that Roblox
 *      process via DuplicateHandle(..., DUPLICATE_CLOSE_SOURCE). This is the
 *      same technique Sysinternals' handle.exe -c uses.
 *   3. If that fails for a process, it tries again on the NEXT 2-second tick.
 *      If it fails a second time in a row for that process, it shows a tray
 *      balloon notification saying so and stops trying for that process.
 *   4. Once a process is handled (or given up on) it is left alone until it
 *      exits. Each additional Roblox window you open gets the same treatment,
 *      so you can run as many instances as you like. The expensive handle
 *      scan only runs once per Roblox process; the recurring 2-second check
 *      is just the cheap process-list lookup.
 *
 * Tray menu:
 *   - Enabled          (pauses/resumes steps 1-3 above)
 *   - Run on startup   (creates/removes a shortcut in the user's Startup
 *                       folder: %APPDATA%\Microsoft\Windows\Start Menu\
 *                       Programs\Startup). Off by default. Nothing is written
 *                       to the registry.
 *   - Exit
 *
 * BUILD (Visual Studio):
 *   Open MultiRobloxWatcher.sln, set Release / x64, Build Solution.
 *   (Ole32.lib and Uuid.lib are pulled in with #pragma comment below.)
 *   No admin rights are required to run it, unless Roblox itself is somehow
 *   running elevated (uncommon).
 *
 * NOTE: This only manipulates a kernel object inside your own local Roblox
 * process on your own machine - it does not touch the network, other users,
 * or Roblox's servers.
 */

#include <Windows.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <map>
#include <string>
#include <vector>
#include "resource.h" // IDI_TRAYICON - add app.rc + resource.h to the project

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Uuid.lib")

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static const wchar_t*  kProcessName      = L"RobloxPlayerBeta.exe";
static const wchar_t*  kHandleNameSuffix = L"ROBLOX_singletonEvent";
static const UINT      kCheckIntervalMs  = 2000;
static const ULONGLONG kCloseDelayMs     = 6000; // wait this long after first seeing Roblox before attempting the close
static const wchar_t*  kAppName          = L"MultiRobloxWatcher";
static const wchar_t*  kWindowClassName  = L"MultiRobloxWatcherWndClass";

// ---------------------------------------------------------------------------
// Minimal NT internals (there's no public header for these - declared by hand)
// ---------------------------------------------------------------------------
typedef LONG NTSTATUS;

typedef struct _UNICODE_STRING_LOCAL {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING_LOCAL;

typedef struct _OBJECT_TYPE_INFORMATION_LOCAL {
    UNICODE_STRING_LOCAL TypeName;
    BYTE Reserved[0x200]; // don't care about the rest of the (large) struct
} OBJECT_TYPE_INFORMATION_LOCAL;

typedef struct _OBJECT_NAME_INFORMATION_LOCAL {
    UNICODE_STRING_LOCAL Name;
} OBJECT_NAME_INFORMATION_LOCAL;

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_LOCAL {
    PVOID     Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG     GrantedAccess;
    USHORT    CreatorBackTraceIndex;
    USHORT    ObjectTypeIndex;
    ULONG     HandleAttributes;
    ULONG     Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_LOCAL;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX_LOCAL {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_LOCAL Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX_LOCAL;

typedef NTSTATUS(NTAPI* PFN_NtQuerySystemInformation)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength);

typedef NTSTATUS(NTAPI* PFN_NtQueryObject)(
    HANDLE Handle,
    ULONG ObjectInformationClass,
    PVOID ObjectInformation,
    ULONG ObjectInformationLength,
    PULONG ReturnLength);

static const ULONG    SystemExtendedHandleInformation_Local = 64;
static const ULONG    ObjectNameInformation_Local           = 1;
static const ULONG    ObjectTypeInformation_Local           = 2;
static const NTSTATUS STATUS_INFO_LENGTH_MISMATCH_LOCAL     = (NTSTATUS)0xC0000004L;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static NOTIFYICONDATAW g_nid = {};
static bool            g_enabled            = true;

// Every running Roblox process is tracked separately, keyed by PID.
struct PidState {
    ULONGLONG firstSeenTick = 0;     // when this PID was first noticed
    int       failCount     = 0;     // consecutive failed close attempts
    bool      done          = false; // closed the event, or gave up - leave it alone
};
static std::map<DWORD, PidState> g_pids;
static UINT            g_msgTaskbarCreated  = 0;     // re-add the tray icon if explorer.exe restarts

static const UINT WM_TRAYICON  = WM_APP + 1;
static const UINT ID_TIMER     = 1;
static const UINT ID_TOGGLE    = 1001;
static const UINT ID_STARTUP   = 1002;
static const UINT ID_EXIT      = 1003;

static const DWORD kTrayBaseFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;

// ---------------------------------------------------------------------------
// Tiny RAII wrapper so no HANDLE is ever leaked on an early-out path.
// ---------------------------------------------------------------------------
struct ScopedHandle {
    HANDLE h = NULL;
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : h(handle) {}
    ~ScopedHandle() { reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    void reset(HANDLE handle = NULL) {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = handle;
    }
    HANDLE* put() { reset(); return &h; }
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};

// ---------------------------------------------------------------------------
// List the PIDs of every running process with the given exe name.
// ---------------------------------------------------------------------------
static std::vector<DWORD> FindProcessesByName(const wchar_t* exeName)
{
    std::vector<DWORD> pids;

    ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return pids;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap.h, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap.h, &pe));
    }
    return pids;
}

// ---------------------------------------------------------------------------
// Enumerate handles owned by targetPid, find the Event object whose name
// ends with nameSuffix, and force-close it inside that process.
// Returns true if the handle was found and successfully closed.
// ---------------------------------------------------------------------------
static bool CloseHandleByNameInProcess(DWORD targetPid, const std::wstring& nameSuffix)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;

    auto NtQuerySystemInformation =
        (PFN_NtQuerySystemInformation)GetProcAddress(ntdll, "NtQuerySystemInformation");
    auto NtQueryObject =
        (PFN_NtQueryObject)GetProcAddress(ntdll, "NtQueryObject");
    if (!NtQuerySystemInformation || !NtQueryObject) return false;

    // Grow a buffer until NtQuerySystemInformation succeeds.
    ULONG bufSize = 1 << 16; // start at 64 KB
    std::vector<BYTE> buffer(bufSize);
    NTSTATUS status = STATUS_INFO_LENGTH_MISMATCH_LOCAL;
    ULONG returnLen = 0;

    for (int attempt = 0; attempt < 8; ++attempt) {
        status = NtQuerySystemInformation(SystemExtendedHandleInformation_Local,
                                          buffer.data(), bufSize, &returnLen);
        if (status == 0) break;
        if (status == STATUS_INFO_LENGTH_MISMATCH_LOCAL) {
            bufSize = returnLen > bufSize ? returnLen + (1 << 16) : bufSize * 2;
            buffer.assign(bufSize, 0);
            continue;
        }
        return false; // unexpected failure
    }
    if (status != 0) return false;

    ScopedHandle hTargetProcess(OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,
                                            FALSE, targetPid));
    if (!hTargetProcess) return false; // e.g. Roblox running elevated & we're not

    auto* info = reinterpret_cast<SYSTEM_HANDLE_INFORMATION_EX_LOCAL*>(buffer.data());

    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const auto& entry = info->Handles[i];
        if ((DWORD)entry.UniqueProcessId != targetPid) continue;

        ScopedHandle hInspect;
        if (!DuplicateHandle(hTargetProcess.h, (HANDLE)entry.HandleValue,
                             GetCurrentProcess(), hInspect.put(),
                             0, FALSE, DUPLICATE_SAME_ACCESS)) {
            continue; // handle not duplicable (rare) - skip it
        }

        // Cheap type check first (safe/fast for every object type).
        BYTE typeBuf[0x400];
        ULONG typeLen = 0;
        if (NtQueryObject(hInspect.h, ObjectTypeInformation_Local, typeBuf, sizeof(typeBuf), &typeLen) != 0)
            continue;

        auto* typeInfo = reinterpret_cast<OBJECT_TYPE_INFORMATION_LOCAL*>(typeBuf);
        if (!typeInfo->TypeName.Buffer || typeInfo->TypeName.Length == 0) continue;

        std::wstring typeName(typeInfo->TypeName.Buffer, typeInfo->TypeName.Length / sizeof(WCHAR));
        if (typeName != L"Event") continue;

        // Only ask for the object's NAME if it's an Event - querying names of
        // other object types (e.g. File/pipe handles) can stall, so we avoid it.
        BYTE nameBuf[0x400];
        ULONG nameLen = 0;
        if (NtQueryObject(hInspect.h, ObjectNameInformation_Local, nameBuf, sizeof(nameBuf), &nameLen) != 0)
            continue;

        auto* nameInfo = reinterpret_cast<OBJECT_NAME_INFORMATION_LOCAL*>(nameBuf);
        if (!nameInfo->Name.Buffer || nameInfo->Name.Length == 0) continue;

        std::wstring objName(nameInfo->Name.Buffer, nameInfo->Name.Length / sizeof(WCHAR));
        if (objName.size() < nameSuffix.size() ||
            objName.compare(objName.size() - nameSuffix.size(), nameSuffix.size(), nameSuffix) != 0)
            continue;

        // Found it: duplicate with CLOSE_SOURCE, which closes Roblox's copy of the
        // handle. Our duplicate is then closed by the ScopedHandle.
        ScopedHandle hCloseDup;
        if (DuplicateHandle(hTargetProcess.h, (HANDLE)entry.HandleValue,
                            GetCurrentProcess(), hCloseDup.put(),
                            0, FALSE, DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)) {
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Tray helpers
// ---------------------------------------------------------------------------
static void ShowBalloon(const wchar_t* title, const wchar_t* text, DWORD flags)
{
    g_nid.uFlags = NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, text, _TRUNCATE);
    g_nid.dwInfoFlags = flags;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = kTrayBaseFlags; // restore, so a later NIM_ADD (explorer restart) re-adds the icon properly
}

// ---------------------------------------------------------------------------
// Run on startup: a shortcut in the per-user Startup folder
//   %APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup
// Resolved with SHGetKnownFolderPath so it works for any user name / redirected
// profile. Nothing is written to the registry.
// ---------------------------------------------------------------------------
static bool GetStartupShortcutPath(std::wstring& out)
{
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, NULL, &folder))) {
        CoTaskMemFree(folder);
        return false;
    }
    out = folder;
    CoTaskMemFree(folder);
    out += L"\\";
    out += kAppName;
    out += L".lnk";
    return true;
}

static bool IsStartupEnabled()
{
    std::wstring path;
    if (!GetStartupShortcutPath(path)) return false;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool SetStartupEnabled(bool enable)
{
    std::wstring lnkPath;
    if (!GetStartupShortcutPath(lnkPath)) return false;

    if (!enable) {
        return DeleteFileW(lnkPath.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH)) return false;

    std::wstring workDir = exePath;
    size_t slash = workDir.find_last_of(L'\\');
    if (slash != std::wstring::npos) workDir.resize(slash);

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link)) || !link)
        return false;

    link->SetPath(exePath);
    link->SetWorkingDirectory(workDir.c_str());
    link->SetDescription(L"MultiRobloxWatcher");

    bool ok = false;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file)) && file) {
        ok = SUCCEEDED(file->Save(lnkPath.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

// Older versions wrote HKCU\...\Run\MultiRobloxWatcher on every launch.
// If that value is still around, carry the "start with Windows" choice over to
// the Startup-folder shortcut and remove the registry entry so it doesn't start twice.
static void MigrateLegacyRegistryStartup()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;

    if (RegQueryValueExW(hKey, kAppName, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        if (SetStartupEnabled(true)) {
            RegDeleteValueW(hKey, kAppName);
        }
    }
    RegCloseKey(hKey);
}

static void ShowTrayMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING | (g_enabled ? MF_CHECKED : MF_UNCHECKED), ID_TOGGLE, L"Enabled");
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED), ID_STARTUP, L"Run on startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // required so the menu closes properly on focus loss
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
// Core 2-second check
//
// Behavior (by design):
//   - Every tick, list all running RobloxPlayerBeta.exe PIDs.
//   - A PID we haven't seen before starts its own countdown. Roblox needs a
//     moment to finish creating the singleton Event, so we wait ~6 seconds
//     after first seeing THAT process before touching it.
//   - On failure: retry once on the next 2s tick. If that also fails, show a
//     balloon warning and stop trying for that process.
//   - Success or gave-up-failure: leave that process alone until it exits.
//   - PIDs that have exited are forgotten, so a later process that happens
//     to reuse the number is treated as new.
// ---------------------------------------------------------------------------
static void CheckRoblox()
{
    if (!g_enabled) return;

    const std::vector<DWORD> running = FindProcessesByName(kProcessName);

    // Forget processes that have exited.
    for (auto it = g_pids.begin(); it != g_pids.end(); ) {
        bool stillRunning = false;
        for (DWORD pid : running) {
            if (pid == it->first) { stillRunning = true; break; }
        }
        it = stillRunning ? std::next(it) : g_pids.erase(it);
    }

    const ULONGLONG now = GetTickCount64();

    for (DWORD pid : running) {
        auto found = g_pids.find(pid);
        if (found == g_pids.end()) {
            // First time we see this process - start its wait.
            PidState st;
            st.firstSeenTick = now;
            g_pids[pid] = st;
            continue;
        }

        PidState& st = found->second;
        if (st.done) continue;
        if (now - st.firstSeenTick < kCloseDelayMs) continue; // still waiting

        if (CloseHandleByNameInProcess(pid, kHandleNameSuffix)) {
            st.done = true;
            st.failCount = 0;
        } else if (++st.failCount >= 2) {
            ShowBalloon(L"MultiRoblox Watcher",
                        L"Couldn't close the ROBLOX singleton handle. A second instance "
                        L"may not start. Try running this app as administrator.",
                        NIIF_WARNING);
            st.done = true; // stop trying for this process
        }
        // else: leave it so the NEXT 2s tick retries.
    }
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        // explorer.exe restarted and the tray was wiped - put the icon back.
        g_nid.uFlags = kTrayBaseFlags;
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, ID_TIMER, kCheckIntervalMs, NULL);
        return 0;

    case WM_TIMER:
        if (wParam == ID_TIMER) CheckRoblox();
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TOGGLE:
            g_enabled = !g_enabled;
            g_pids.clear(); // re-evaluate every running process when re-enabled
            break;
        case ID_STARTUP:
            if (!SetStartupEnabled(!IsStartupEnabled())) {
                ShowBalloon(L"MultiRoblox Watcher",
                            L"Couldn't update the startup shortcut.",
                            NIIF_WARNING);
            }
            break;
        case ID_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // Prevent running two copies of the watcher itself.
    ScopedHandle singleInstance(CreateMutexW(NULL, TRUE, L"MultiRobloxWatcher_SingleInstanceMutex"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    bool comOk = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));

    // One-time carry-over from the old registry-based startup (no-op otherwise).
    // If the shortcut already exists, refresh it so it follows the exe if it was moved.
    MigrateLegacyRegistryStartup();
    if (IsStartupEnabled()) SetStartupEnabled(true);

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);

    // Hidden top-level window - we never show it, we only need an HWND to
    // receive tray/timer/menu messages. (A message-only window (HWND_MESSAGE)
    // can't receive the broadcast "TaskbarCreated" message, so this isn't one.)
    HWND hwnd = CreateWindowExW(0, kWindowClassName, kAppName, WS_POPUP,
                                0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        if (comOk) CoUninitialize();
        return 1;
    }

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = kTrayBaseFlags;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_TRAYICON));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION); // fallback if resource missing
    wcsncpy_s(g_nid.szTip, L"MultiRoblox Watcher", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (comOk) CoUninitialize();
    return (int)msg.wParam;
}
