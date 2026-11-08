// ============================================================================
//  Winget Update Manager
//  A small, portable Win32 GUI for checking and applying winget updates.
//
//  - Checks for updates automatically on startup
//  - Lists available updates with check boxes (all pre-checked)
//  - Updates only the selected packages, or everything at once
//  - Shows the raw winget output as a terminal-style log
//  - Follows the Windows light/dark theme (or forces one in Settings)
//  - Optional "always run as administrator"
//
//  Built with MinGW-w64 (static), no runtime dependencies. See build.ps1.
// ============================================================================

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cstring>

#include "parser.h"
#include "resource.h"
#include "settings.h"
#include "theme.h"

#if defined(_MSC_VER)
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#endif

namespace {

// ---------------------------------------------------------------- messages
const UINT WM_APP_LOGTEXT    = WM_APP + 1;  // lParam: wchar_t* (owned by UI)
const UINT WM_APP_UPDATES    = WM_APP + 2;  // lParam: std::vector<Package>*
const UINT WM_APP_FINISHED   = WM_APP + 3;  // lParam: wchar_t* note or null
const UINT WM_APP_STATUS     = WM_APP + 4;  // lParam: wchar_t* (owned by UI)
const UINT WM_APP_STARTCHECK = WM_APP + 5;

const wchar_t* const kTitle = L"Winget Update Manager";

const wchar_t* const kCheckArgs =
    L"upgrade --include-unknown --accept-source-agreements --disable-interactivity";

const wchar_t* const kUpdateAllArgs =
    L"upgrade --all --include-unknown --accept-package-agreements "
    L"--accept-source-agreements --disable-interactivity";

// Trim whole leading lines from the log once it exceeds this size, so a long
// `winget upgrade` cannot grow the edit control without bound.
const int kMaxLogChars = 2 * 1024 * 1024;

// ---------------------------------------------------------------- data types
using winget::Package;

struct WorkItem {
    bool updateAll = false;
    std::vector<std::wstring> ids;
};

// ---------------------------------------------------------------- globals
HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
HWND g_list = nullptr;
HWND g_log = nullptr;
HWND g_status = nullptr;
HWND g_btnCheck = nullptr;
HWND g_btnUpdate = nullptr;
HWND g_btnAll = nullptr;
HWND g_btnSelAll = nullptr;
HWND g_btnSelNone = nullptr;
HWND g_btnSettings = nullptr;

HICON g_iconLarge = nullptr;
HICON g_iconSmall = nullptr;

HFONT g_fontUI = nullptr;
HFONT g_fontMono = nullptr;
HFONT g_fontHeader = nullptr;

settings::Values g_settings;
std::vector<Package> g_packages;

// UI flags shared with worker threads. std::atomic makes the cross-thread
// access well-defined; all usage is plain 0/1 assignment and read.
std::atomic<int> g_busy{0};    // 0 = idle, 1 = a worker is running
std::atomic<int> g_alive{1};   // 0 once the window is gone
bool g_elevated = false;
bool g_filling = false;      // suppress notifications while populating

// Groups every winget child process so a close during an update can either
// cancel the run (TerminateJobObject) or release it to finish in the
// background (no kill-on-close, so closing the handle is harmless).
HANDLE g_job = nullptr;

struct ButtonVisual {
    bool primary = false;
    bool hover = false;
    bool pressed = false;
};

void RefreshButtons();
void UpdateStatusText();
void ApplyThemeLive();
void PostLog(const std::wstring& text);
int DarkMessageBox(HWND owner, const wchar_t* text, const wchar_t* caption,
                   unsigned int type);

LRESULT CALLBACK HeaderSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                UINT_PTR id, DWORD_PTR ref);
LRESULT CALLBACK ThemeButtonSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR id, DWORD_PTR ref);
LRESULT CALLBACK GroupBoxSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR id, DWORD_PTR ref);
LRESULT CALLBACK ModernButtonSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR id, DWORD_PTR ref);

// ---------------------------------------------------------------- helpers
// Formats an unsigned integer without pulling in the locale-aware
// std::to_wstring machinery.
std::wstring NumW(unsigned long long value) {
    wchar_t buf[24];
    int i = 23;
    buf[i] = L'\0';
    do {
        buf[--i] = static_cast<wchar_t>(L'0' + value % 10);
        value /= 10;
    } while (value != 0);
    return std::wstring(buf + i);
}

wchar_t* DupStr(const std::wstring& s) {
    wchar_t* p = new wchar_t[s.size() + 1];
    if (!s.empty()) std::memcpy(p, s.c_str(), s.size() * sizeof(wchar_t));
    p[s.size()] = L'\0';
    return p;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// Returns the DPI for the monitor containing the window (Per-Monitor V2 aware),
// falling back to the system DPI or 96.
int GetDpiForHwnd(HWND hwnd) {
    using FnGetDpiForWindow = UINT (WINAPI*)(HWND);
    static auto fnGetDpiForWindow = reinterpret_cast<FnGetDpiForWindow>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fnGetDpiForWindow && hwnd) {
        UINT dpi = fnGetDpiForWindow(hwnd);
        if (dpi > 0) return static_cast<int>(dpi);
    }
    using FnGetDpiForSystem = UINT (WINAPI*)(void);
    static auto fnGetDpiForSystem = reinterpret_cast<FnGetDpiForSystem>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem")));
    if (fnGetDpiForSystem) {
        UINT dpi = fnGetDpiForSystem();
        if (dpi > 0) return static_cast<int>(dpi);
    }
    HDC dc = GetDC(hwnd);
    if (!dc) return 96;
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    return (dpi > 0) ? dpi : 96;
}

// Scales a 96-dpi design size by the window's monitor DPI.
int ScaleForDpi(HWND hwnd, int value) {
    const int dpi = GetDpiForHwnd(hwnd);
    return (dpi > 0 && dpi != 96) ? MulDiv(value, dpi, 96) : value;
}

bool RelaunchElevated() {
    wchar_t path[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

bool EffectiveDark() {
    switch (g_settings.theme) {
        case settings::ThemeDark:  return true;
        case settings::ThemeLight: return false;
        default:                   return theme::SystemPrefersDark();
    }
}

void ApplyListColors() {
    if (!g_list) return;
    // A themed ListView still paints its empty area with COLOR_WINDOW, so the
    // background/text colours have to be set explicitly for dark mode.
    ListView_SetBkColor(g_list, theme::FieldColor());
    ListView_SetTextBkColor(g_list, theme::FieldColor());
    ListView_SetTextColor(g_list, theme::FieldTextColor());
    RedrawWindow(g_list, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void ApplyThemeLive() {
    theme::Init(EffectiveDark());
    if (g_hwnd) {
        theme::ApplyWindow(g_hwnd);
        theme::StyleTree(g_hwnd);
        ApplyListColors();
        RedrawWindow(g_hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }
    UpdateStatusText();
}

// ---------------------------------------------------------------- UI posting
using winget::StripAnsi;
using winget::Utf8ToWide;

void PostLog(const std::wstring& text) {
    if (!g_alive.load() || !g_hwnd) return;
    wchar_t* p = DupStr(text);
    if (!PostMessageW(g_hwnd, WM_APP_LOGTEXT, 0, reinterpret_cast<LPARAM>(p))) delete[] p;
}

void PostStatus(const std::wstring& text) {
    if (!g_alive.load() || !g_hwnd) return;
    wchar_t* p = DupStr(text);
    if (!PostMessageW(g_hwnd, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(p))) delete[] p;
}

// ---------------------------------------------------------------- log output
void AppendLog(const std::wstring& text) {
    if (!g_log || text.empty()) return;

    const int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));

    std::wstring chunk = text;
    if (chunk.size() < 2 || chunk.compare(chunk.size() - 2, 2, L"\r\n") != 0) chunk += L"\r\n";

    SendMessageW(g_log, EM_SETREADONLY, FALSE, 0);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(chunk.c_str()));
    SendMessageW(g_log, EM_SETREADONLY, TRUE, 0);

    // Trim whole leading lines once the text is past the cap, so a long
    // `winget upgrade` cannot grow the log without bound.
    const int grown = GetWindowTextLengthW(g_log);
    if (grown > kMaxLogChars) {
        const int line = static_cast<int>(SendMessageW(
            g_log, EM_LINEFROMCHAR, static_cast<WPARAM>(grown - kMaxLogChars), 0));
        const int cut = static_cast<int>(SendMessageW(
            g_log, EM_LINEINDEX, static_cast<WPARAM>(line + 1), 0));
        if (cut > 0) {
            SendMessageW(g_log, EM_SETREADONLY, FALSE, 0);
            SendMessageW(g_log, EM_SETSEL, 0, static_cast<WPARAM>(cut));
            SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
            SendMessageW(g_log, EM_SETREADONLY, TRUE, 0);
        }
    }

    // Trimming invalidates the caret selection placed before the append.
    const int end = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, static_cast<WPARAM>(end), static_cast<LPARAM>(end));
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

// ---------------------------------------------------------------- process I/O
// Turns a raw winget byte stream into clean text lines.
//
//  * \r\n  -> one line break
//  * bare \r (progress redraw) -> the pending line is replaced; a throttled
//    sample is kept so the log still shows progress without flooding
//  * ANSI escape sequences and control characters are stripped
class OutputParser {
public:
    std::string* collect = nullptr;  // optional: cleaned text for parsing

    void Feed(const char* data, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const char c = data[i];
            if (crPending_) {
                crPending_ = false;
                if (c == '\n') { EmitLine(); continue; }
                OnBareCarriageReturn();
            }
            if (c == '\r') { crPending_ = true; continue; }
            if (c == '\n') { EmitLine(); continue; }
            pending_.push_back(c);
            if (pending_.size() > 4000) EmitLine();
        }
        FlushBatch();
    }

    void Finish() {
        if (crPending_) { crPending_ = false; EmitLine(); }
        EmitLine();
        FlushBatch();
    }

private:
    std::string pending_;
    std::wstring batch_;
    bool crPending_ = false;
    bool firstLine_ = true;
    ULONGLONG lastProgress_ = 0;

    void OnBareCarriageReturn() {
        if (pending_.empty()) return;
        const ULONGLONG now = GetTickCount64();
        if (now - lastProgress_ >= 400) {
            EmitLine();
            lastProgress_ = now;
        } else {
            pending_.clear();
        }
    }

    void EmitLine() {
        std::string line;
        line.swap(pending_);

        // drop a UTF-8 BOM if winget ever emits one
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }

        std::string clean = StripAnsi(line);
        while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\t')) clean.pop_back();

        if (firstLine_) {
            firstLine_ = false;
            if (clean.empty()) return;
        }

        if (collect) {
            collect->append(clean);
            collect->push_back('\n');
        }
        AddToBatch(clean);
    }

    void AddToBatch(const std::string& clean) {
        const std::wstring w = Utf8ToWide(clean);
        if (!batch_.empty()) batch_ += L"\r\n";
        batch_ += w;
        if (batch_.size() > 24000) FlushBatch();
    }

    void FlushBatch() {
        if (batch_.empty()) return;
        if (g_alive.load() && g_hwnd) {
            wchar_t* p = DupStr(batch_);
            if (!PostMessageW(g_hwnd, WM_APP_LOGTEXT, 0, reinterpret_cast<LPARAM>(p))) delete[] p;
        }
        batch_.clear();
    }
};

// Runs a command, streaming combined stdout/stderr into `sink`.
// Returns false only when the process could not be started at all.
bool RunProcess(const std::wstring& cmd, OutputParser& sink, DWORD& exitCode, DWORD& createErr) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { createErr = GetLastError(); return false; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    const BOOL ok = CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    createErr = ok ? 0 : GetLastError();

    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    if (!ok) {
        CloseHandle(rd);
        return false;
    }

    CloseHandle(pi.hThread);

    // Add the child to the job so a close during the run can cancel it.
    // The cmd.exe fallback wrapper joins the job too, and winget inherits
    // the association from it.
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);

    char buf[8192];
    DWORD n = 0;
    while (g_alive.load() && ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
        sink.Feed(buf, static_cast<size_t>(n));
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    exitCode = ec;

    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    sink.Finish();
    return true;
}

bool RunWinget(const std::wstring& args, OutputParser& sink, DWORD& exitCode) {
    const std::wstring cmd = L"winget.exe " + args;
    DWORD err = 0;
    if (RunProcess(cmd, sink, exitCode, err)) return true;

    // App-execution aliases normally launch directly; fall back to the shell.
    PostLog(L"[!] Could not start winget directly (error " + NumW(err) +
            L"), retrying via cmd.exe");
    const std::wstring wrapped = L"cmd.exe /d /s /c \"" + cmd + L"\"";
    if (RunProcess(wrapped, sink, exitCode, err)) return true;

    PostLog(L"[!] Failed to launch winget (error " + NumW(err) + L").");
    PostLog(L"[!] Make sure Windows Package Manager (winget) is installed.");
    return false;
}

// ---------------------------------------------------------------- workers
void LogCommand(const std::wstring& args) {
    PostLog(L"");
    PostLog(L"> winget " + args);
}

bool DoCheck(std::wstring& note) {
    PostStatus(L"Checking for updates...");

    std::string captured;
    OutputParser sink;
    sink.collect = &captured;
    LogCommand(kCheckArgs);

    DWORD exitCode = 0;
    if (!RunWinget(kCheckArgs, sink, exitCode)) {
        note = L"Could not run winget.";
        return false;
    }

    std::vector<Package>* result =
        new std::vector<Package>(winget::ParseUpgradeOutput(captured));
    if (!g_alive.load() || !g_hwnd) {
        delete result;
        return false;
    }
    PostMessageW(g_hwnd, WM_APP_UPDATES, 0, reinterpret_cast<LPARAM>(result));
    return true;
}

bool DoUpdateAll(std::wstring& note) {
    PostStatus(L"Updating all packages...");
    LogCommand(kUpdateAllArgs);

    OutputParser sink;
    DWORD exitCode = 0;
    if (!RunWinget(kUpdateAllArgs, sink, exitCode)) {
        note = L"Could not run winget.";
        return false;
    }
    return true;
}

bool DoUpdateIds(const std::vector<std::wstring>& ids, std::wstring& note) {
    const int total = static_cast<int>(ids.size());
    int index = 0;
    for (const std::wstring& id : ids) {
        ++index;
        PostStatus(L"Updating package " + NumW(index) + L" of " +
                   NumW(total) + L"...");

        const std::wstring args =
            L"upgrade --id " + winget::EscapeArg(id) + L" --exact --include-unknown "
            L"--accept-package-agreements --accept-source-agreements "
            L"--disable-interactivity";
        LogCommand(args);

        OutputParser sink;
        DWORD exitCode = 0;
        if (!RunWinget(args, sink, exitCode)) {
            note = L"Could not run winget.";
            return false;
        }
    }
    return true;
}

DWORD WINAPI WorkerThread(LPVOID param) {
    WorkItem* work = static_cast<WorkItem*>(param);
    std::wstring note;

    if (work->updateAll) {
        if (DoUpdateAll(note)) {
            note.clear();
            PostStatus(L"Refreshing package list...");
            DoCheck(note);
        }
    } else if (!work->ids.empty()) {
        if (DoUpdateIds(work->ids, note)) {
            note.clear();
            PostStatus(L"Refreshing package list...");
            DoCheck(note);
        }
    } else {
        DoCheck(note);
    }

    if (g_alive.load() && g_hwnd) {
        PostMessageW(g_hwnd, WM_APP_FINISHED, 0,
                     note.empty() ? 0 : reinterpret_cast<LPARAM>(DupStr(note)));
    }

    delete work;
    return 0;
}

void BeginWork(WorkItem* work, const std::wstring& status) {
    int expected = 0;
    if (!g_busy.compare_exchange_strong(expected, 1)) {
        // A check or update is already running. Say so instead of dropping the
        // request without a trace - the button stays enabled, so this is how a
        // second click can land here.
        PostStatus(L"Another operation is running - try again once it finishes.");
        delete work;
        return;
    }

    if (!g_job) {
        // No kill-on-close limit: releasing the job (plain CloseHandle at
        // destroy) must let a running winget finish on its own.
        g_job = CreateJobObjectW(nullptr, nullptr);
    }

    SetWindowTextW(g_status, status.c_str());
    RefreshButtons();

    HANDLE thread = CreateThread(nullptr, 0, WorkerThread, work, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        g_busy.store(0, std::memory_order_release);
        delete work;
        RefreshButtons();
    }
}

// ---------------------------------------------------------------- list / UI state
int CountChecked() {
    int count = 0;
    const int n = ListView_GetItemCount(g_list);
    for (int i = 0; i < n; ++i) {
        if (ListView_GetCheckState(g_list, i)) ++count;
    }
    return count;
}

void UpdateStatusText() {
    if (g_busy.load() || !g_status) return;

    const int total = ListView_GetItemCount(g_list);
    const int checked = CountChecked();

    std::wstring s;
    if (total == 0) {
        s = L"No updates available.";
    } else {
        s = NumW(total);
        s += (total == 1) ? L" update available, " : L" updates available, ";
        s += NumW(checked);
        s += L" selected.";
    }
    if (!g_elevated) s += L"   (standard user)";
    SetWindowTextW(g_status, s.c_str());
}

void RefreshButtons() {
    const bool idle = (g_busy.load() == 0);
    const int checked = CountChecked();
    const int total = ListView_GetItemCount(g_list);

    EnableWindow(g_btnCheck, idle);
    EnableWindow(g_btnUpdate, idle && checked > 0);
    EnableWindow(g_btnAll, idle && total > 0);
    EnableWindow(g_btnSelAll, idle && total > 0);
    EnableWindow(g_btnSelNone, idle && checked > 0);
    EnableWindow(g_btnSettings, idle);
}

void LayoutColumns() {
    if (!g_list) return;
    RECT rc{};
    GetClientRect(g_list, &rc);
    const int total = rc.right - rc.left;

    const int wId = ScaleForDpi(g_list, 300), wCur = ScaleForDpi(g_list, 110),
              wAvail = ScaleForDpi(g_list, 110), wSrc = ScaleForDpi(g_list, 100);
    int wName = total - (wId + wCur + wAvail + wSrc) - ScaleForDpi(g_list, 24);
    if (wName < ScaleForDpi(g_list, 150)) wName = ScaleForDpi(g_list, 150);

    ListView_SetColumnWidth(g_list, 0, wName);
    ListView_SetColumnWidth(g_list, 1, wId);
    ListView_SetColumnWidth(g_list, 2, wCur);
    ListView_SetColumnWidth(g_list, 3, wAvail);
    ListView_SetColumnWidth(g_list, 4, wSrc);
}

void FillList() {
    g_filling = true;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);

    for (size_t i = 0; i < g_packages.size(); ++i) {
        const Package& p = g_packages[i];

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(p.name.c_str());
        item.lParam = static_cast<LPARAM>(i);

        const int row = ListView_InsertItem(g_list, &item);
        if (row < 0) continue;

        ListView_SetItemText(g_list, row, 1, const_cast<wchar_t*>(p.id.c_str()));
        ListView_SetItemText(g_list, row, 2, const_cast<wchar_t*>(p.current.c_str()));
        ListView_SetItemText(g_list, row, 3, const_cast<wchar_t*>(p.available.c_str()));
        ListView_SetItemText(g_list, row, 4, const_cast<wchar_t*>(p.source.c_str()));
        ListView_SetCheckState(g_list, row, TRUE);
    }

    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
    g_filling = false;

    UpdateStatusText();
    RefreshButtons();
}

std::vector<std::wstring> CollectSelectedIds() {
    std::vector<std::wstring> ids;
    const int n = ListView_GetItemCount(g_list);
    for (int i = 0; i < n; ++i) {
        if (!ListView_GetCheckState(g_list, i)) continue;

        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        if (!ListView_GetItem(g_list, &item)) continue;

        const size_t index = static_cast<size_t>(item.lParam);
        if (index < g_packages.size()) ids.push_back(g_packages[index].id);
    }
    return ids;
}

void SetAllChecks(BOOL checked) {
    const int n = ListView_GetItemCount(g_list);
    g_filling = true;
    for (int i = 0; i < n; ++i) ListView_SetCheckState(g_list, i, checked);
    g_filling = false;
    UpdateStatusText();
    RefreshButtons();
}

// ---------------------------------------------------------------- commands
void StartCheck() {
    BeginWork(new WorkItem(), L"Checking for updates...");
}

void StartSelected() {
    std::vector<std::wstring> ids = CollectSelectedIds();
    if (ids.empty()) {
        DarkMessageBox(g_hwnd, L"Select at least one package to update.",
                       kTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }
    WorkItem* work = new WorkItem();
    work->ids = std::move(ids);
    BeginWork(work, L"Updating selected packages...");
}

void StartAll() {
    const int total = ListView_GetItemCount(g_list);
    const std::wstring question = L"Update all " + NumW(total) + L" packages now?";

    if (DarkMessageBox(g_hwnd, question.c_str(), L"Update all",
                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    WorkItem* work = new WorkItem();
    work->updateAll = true;
    BeginWork(work, L"Updating all packages...");
}

// ---------------------------------------------------------------- settings
INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        RECT dlgRect{}, ownerRect{};
        GetWindowRect(dlg, &dlgRect);
        GetWindowRect(g_hwnd, &ownerRect);
        const int x = ownerRect.left +
                      ((ownerRect.right - ownerRect.left) - (dlgRect.right - dlgRect.left)) / 2;
        const int y = ownerRect.top +
                      ((ownerRect.bottom - ownerRect.top) - (dlgRect.bottom - dlgRect.top)) / 2;
        SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        CheckRadioButton(dlg, IDC_THEME_SYSTEM, IDC_THEME_DARK,
                         g_settings.theme == settings::ThemeDark  ? IDC_THEME_DARK
                         : g_settings.theme == settings::ThemeLight ? IDC_THEME_LIGHT
                                                                    : IDC_THEME_SYSTEM);
        CheckDlgButton(dlg, IDC_RUN_ADMIN,
                       g_settings.runAsAdmin ? BST_CHECKED : BST_UNCHECKED);

        SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_iconSmall));
        theme::ApplyWindow(dlg);
        theme::StyleTree(dlg);
        // A themed radio/check box draws its label in black as soon as it has
        // keyboard focus, which is unreadable on the dark background, so these
        // controls are painted by ThemeButtonSubclass instead.
        const int themedButtons[] = {IDC_THEME_SYSTEM, IDC_THEME_LIGHT,
                                     IDC_THEME_DARK, IDC_RUN_ADMIN};
        for (int id : themedButtons) {
            if (HWND button = GetDlgItem(dlg, id)) {
                SetWindowSubclass(button, ThemeButtonSubclass, static_cast<UINT_PTR>(id), 0);
            }
        }

        // Match the main window's typography and flat buttons.
        EnumChildWindows(dlg, [](HWND child, LPARAM) -> BOOL {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUI), TRUE);
            // The group box has no dialog ID, so it is found by class/style.
            if ((GetWindowLongW(child, GWL_STYLE) & 0x0F) == BS_GROUPBOX) {
                SetWindowSubclass(child, GroupBoxSubclass, 2, 0);
            }
            return TRUE;
        }, 0);
        const int dialogButtons[] = {IDOK, IDCANCEL};
        for (int id : dialogButtons) {
            if (HWND button = GetDlgItem(dlg, id)) {
                auto* visual = new ButtonVisual();
                visual->primary = (id == IDOK);
                SetWindowSubclass(button, ModernButtonSubclass,
                                  static_cast<UINT_PTR>(id),
                                  reinterpret_cast<DWORD_PTR>(visual));
            }
        }

        // Move the initial focus to OK once the dialog is shown; SetFocus
        // during WM_INITDIALOG is ignored before the dialog is visible, which
        // would leave the focus (and its rectangle) on the first radio.
        PostMessageW(dlg, WM_NEXTDLGCTL,
                     reinterpret_cast<WPARAM>(GetDlgItem(dlg, IDOK)), TRUE);
        return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        // Radio buttons, check boxes and group boxes colour their text through
        // WM_CTLCOLORBTN; without this they keep the default black and become
        // unreadable on the dark background.
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, theme::TextColor());
        SetBkColor(hdc, theme::WindowColor());
        return reinterpret_cast<INT_PTR>(theme::WindowBrush());
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            settings::Values values;
            if (IsDlgButtonChecked(dlg, IDC_THEME_DARK) == BST_CHECKED) {
                values.theme = settings::ThemeDark;
            } else if (IsDlgButtonChecked(dlg, IDC_THEME_LIGHT) == BST_CHECKED) {
                values.theme = settings::ThemeLight;
            } else {
                values.theme = settings::ThemeSystem;
            }
            values.runAsAdmin = IsDlgButtonChecked(dlg, IDC_RUN_ADMIN) == BST_CHECKED;

            const bool themeChanged = (values.theme != g_settings.theme);
            const bool wantElevation = values.runAsAdmin && !g_elevated;

            g_settings = values;
            settings::Save(g_settings);
            EndDialog(dlg, IDOK);

            if (themeChanged) ApplyThemeLive();
            if (wantElevation && RelaunchElevated()) {
                PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            }
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;

    default:
        break;
    }
    return FALSE;
}

void ShowSettings() {
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), g_hwnd, SettingsProc, 0);
}

// ---------------------------------------------------------------- controls
void DeleteFonts() {
    if (g_fontUI && g_fontUI != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(g_fontUI);
        g_fontUI = nullptr;
    }
    if (g_fontMono && g_fontMono != g_fontUI) {
        DeleteObject(g_fontMono);
        g_fontMono = nullptr;
    }
    if (g_fontHeader && g_fontHeader != g_fontUI) {
        DeleteObject(g_fontHeader);
        g_fontHeader = nullptr;
    }
}

void CreateFonts(HWND hwnd) {
    DeleteFonts();

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);

    const int dpi = GetDpiForHwnd(hwnd);

    using FnSystemParametersInfoForDpi = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static auto fnSPIForDpi = reinterpret_cast<FnSystemParametersInfoForDpi>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi")));

    BOOL ok = FALSE;
    if (fnSPIForDpi) {
        ok = fnSPIForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, static_cast<UINT>(dpi));
    }
    if (!ok) {
        ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        if (ok && dpi != 96) {
            metrics.lfMessageFont.lfHeight = MulDiv(metrics.lfMessageFont.lfHeight, dpi, 96);
        }
    }

    if (ok) {
        LOGFONTW ui = metrics.lfMessageFont;
        const int tweak = MulDiv(1, dpi, 96);
        ui.lfHeight = (ui.lfHeight < 0) ? (ui.lfHeight - tweak) : (ui.lfHeight + tweak);
        wcscpy(ui.lfFaceName, L"Segoe UI");
        g_fontUI = CreateFontIndirectW(&ui);

        LOGFONTW mono = metrics.lfMessageFont;
        mono.lfHeight = (mono.lfHeight < 0) ? (mono.lfHeight - tweak) : (mono.lfHeight + tweak);
        mono.lfWeight = FW_NORMAL;
        wcscpy(mono.lfFaceName, L"Consolas");
        g_fontMono = CreateFontIndirectW(&mono);

        LOGFONTW header = ui;
        const int headerTweak = MulDiv(2, dpi, 96);
        header.lfHeight = (metrics.lfMessageFont.lfHeight < 0)
                              ? (metrics.lfMessageFont.lfHeight - headerTweak)
                              : (metrics.lfMessageFont.lfHeight + headerTweak);
        header.lfWeight = FW_SEMIBOLD;
        wcscpy(header.lfFaceName, L"Segoe UI Semibold");
        g_fontHeader = CreateFontIndirectW(&header);
    } else {
        g_fontUI = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        g_fontMono = g_fontUI;
        g_fontHeader = g_fontUI;
    }
}

void PaintModernButton(HWND hwnd, HDC hdc, const ButtonVisual& v) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    const bool disabled = IsWindowEnabled(hwnd) == FALSE;
    const bool focused = GetFocus() == hwnd;

    COLORREF background;
    COLORREF border;
    COLORREF text;
    if (disabled) {
        background = theme::WindowColor();
        border = theme::BorderColor();
        text = theme::MutedTextColor();
    } else if (v.primary) {
        background = v.pressed ? theme::AccentPressedColor()
                     : v.hover ? theme::AccentHoverColor()
                               : theme::AccentColor();
        border = background;
        text = RGB(255, 255, 255);
    } else {
        background = v.pressed ? theme::PressedColor()
                     : v.hover ? theme::HoverColor()
                               : theme::WindowColor();
        border = (v.hover || focused) ? theme::AccentColor() : theme::BorderColor();
        text = theme::TextColor();
    }

    const int radius = ScaleForDpi(hwnd, 10);  // corner ellipse diameter
    HBRUSH backgroundBrush = CreateSolidBrush(background);
    HPEN borderPen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(hdc, backgroundBrush);
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(backgroundBrush);
    DeleteObject(borderPen);

    wchar_t label[128] = {0};
    GetWindowTextW(hwnd, label, 128);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text);
    HGDIOBJ oldFont = SelectObject(hdc, g_fontUI);
    RECT textRect = rc;
    textRect.top -= 1;  // optical centring for rounded text
    DrawTextW(hdc, label, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);

    if (focused && !disabled) {
        RECT ring = rc;
        const int ringInset = ScaleForDpi(hwnd, 3);
        InflateRect(&ring, -ringInset, -ringInset);
        HPEN ringPen = CreatePen(PS_SOLID, 1,
                                 v.primary ? RGB(255, 255, 255) : theme::AccentColor());
        HGDIOBJ oldRingPen = SelectObject(hdc, ringPen);
        HGDIOBJ oldRingBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, ring.left, ring.top, ring.right, ring.bottom, radius, radius);
        SelectObject(hdc, oldRingPen);
        SelectObject(hdc, oldRingBrush);
        DeleteObject(ringPen);
    }
}

// Flat, rounded, theme-aware buttons with hover and pressed feedback.
LRESULT CALLBACK ModernButtonSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR, DWORD_PTR ref) {
    auto* visual = reinterpret_cast<ButtonVisual*>(ref);
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, theme::WindowBrush());
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        FillRect(hdc, &ps.rcPaint, theme::WindowBrush());
        PaintModernButton(hwnd, hdc, *visual);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!visual->hover) {
            visual->hover = true;
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        visual->hover = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_LBUTTONDOWN:
        visual->pressed = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (visual->pressed) {
            visual->pressed = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        delete visual;
        RemoveWindowSubclass(hwnd, ModernButtonSubclass, 0);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void UpdateListRowHeight(HWND parent) {
    if (!g_list) return;
    HDC dc = GetDC(g_list);
    HGDIOBJ oldFont = SelectObject(dc, g_fontUI);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, oldFont);
    ReleaseDC(g_list, dc);

    const int rowH = tm.tmHeight + ScaleForDpi(parent, 12);
    HIMAGELIST oldList = ListView_GetImageList(g_list, LVSIL_SMALL);
    if (HIMAGELIST rows = ImageList_Create(1, rowH, ILC_COLOR32, 0, 1)) {
        ListView_SetImageList(g_list, rows, LVSIL_SMALL);
    }
    if (oldList) ImageList_Destroy(oldList);
}

void ApplyFonts() {
    HWND children[] = {g_status, g_list, g_log, g_btnCheck, g_btnUpdate, g_btnAll,
                       g_btnSelAll, g_btnSelNone, g_btnSettings};
    for (HWND child : children) {
        if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUI), TRUE);
    }
    if (g_log) SendMessageW(g_log, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontMono), TRUE);
    if (HWND header = ListView_GetHeader(g_list)) {
        SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontHeader), TRUE);
    }
}

void CreateControls(HWND parent) {
    CreateFonts(parent);

    g_status = CreateWindowExW(0, L"STATIC", L"Starting...",
                               WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                               0, 0, 0, 0, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)),
                               g_inst, nullptr);

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                 LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                             0, 0, 0, 0, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)),
                             g_inst, nullptr);
    ListView_SetExtendedListViewStyle(
        g_list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES |
                    LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

    // A dummy 1-px-wide image list raises the row height, giving the list a
    // roomier, more modern feel without owner-drawing every row.
    UpdateListRowHeight(parent);

    struct Column { const wchar_t* text; int width; };
    const Column columns[] = {
        {L"Package", 260}, {L"Id", 300}, {L"Current", 110},
        {L"Available", 110}, {L"Source", 100},
    };
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(columns[i].text);
        column.cx = columns[i].width;
        column.iSubItem = i;
        ListView_InsertColumn(g_list, i, &column);
    }

    g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_LEFT,
                            0, 0, 0, 0, parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)),
                            g_inst, nullptr);
    SendMessageW(g_log, EM_SETLIMITTEXT, static_cast<WPARAM>(0x7FFFFFFE), 0);

    auto button = [&](const wchar_t* text, int id, bool primary) -> HWND {
        HWND created = CreateWindowExW(0, L"BUTTON", text,
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       0, 0, 0, 0, parent,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       g_inst, nullptr);
        if (created) {
            auto* visual = new ButtonVisual();
            visual->primary = primary;
            SetWindowSubclass(created, ModernButtonSubclass,
                              static_cast<UINT_PTR>(id),
                              reinterpret_cast<DWORD_PTR>(visual));
        }
        return created;
    };

    g_btnCheck    = button(L"Check for updates", IDC_BTN_CHECK, false);
    g_btnUpdate   = button(L"Update selected", IDC_BTN_UPDATE, true);
    g_btnAll      = button(L"Update all", IDC_BTN_ALL, false);
    g_btnSelAll   = button(L"Select all", IDC_BTN_SEL_ALL, false);
    g_btnSelNone  = button(L"Select none", IDC_BTN_SEL_NONE, false);
    g_btnSettings = button(L"Settings", IDC_BTN_SETTINGS, false);

    ApplyFonts();

    theme::StyleTree(parent);
    ApplyListColors();

    // The header is created lazily by the ListView, so subclass it after the
    // columns exist.
    if (HWND header = ListView_GetHeader(g_list)) {
        SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontHeader), TRUE);
        SetWindowSubclass(header, HeaderSubclass, 1, 0);
    }
}

void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int W = rc.right - rc.left;
    const int H = rc.bottom - rc.top;

    const int pad = ScaleForDpi(hwnd, 16);
    const int statusH = ScaleForDpi(hwnd, 22);
    const int btnH = ScaleForDpi(hwnd, 36);
    const int gap = ScaleForDpi(hwnd, 10);
    const int btnGap = ScaleForDpi(hwnd, 8);

    int logH = static_cast<int>(H * 0.26);
    const int minLogH = ScaleForDpi(hwnd, 96);
    const int maxLogH = ScaleForDpi(hwnd, 280);
    if (logH < minLogH) logH = minLogH;
    if (logH > maxLogH) logH = maxLogH;

    int y = pad;
    MoveWindow(g_status, pad, y, W - 2 * pad, statusH, TRUE);
    y += statusH + ScaleForDpi(hwnd, 8);

    const int btnY = H - pad - logH - gap - btnH;
    int listH = btnY - gap - y;
    if (listH < ScaleForDpi(hwnd, 60)) listH = ScaleForDpi(hwnd, 60);
    MoveWindow(g_list, pad, y, W - 2 * pad, listH, TRUE);

    int bx = pad;
    auto place = [&](HWND btn, int w) {
        MoveWindow(btn, bx, btnY, w, btnH, TRUE);
        bx += w + btnGap;
    };
    place(g_btnCheck, ScaleForDpi(hwnd, 150));
    place(g_btnUpdate, ScaleForDpi(hwnd, 145));
    place(g_btnAll, ScaleForDpi(hwnd, 110));
    bx += ScaleForDpi(hwnd, 18);
    place(g_btnSelAll, ScaleForDpi(hwnd, 100));
    place(g_btnSelNone, ScaleForDpi(hwnd, 110));

    MoveWindow(g_btnSettings, W - pad - ScaleForDpi(hwnd, 110), btnY,
               ScaleForDpi(hwnd, 110), btnH, TRUE);

    const int logY = btnY + btnH + gap;
    MoveWindow(g_log, pad, logY, W - 2 * pad, H - pad - logY, TRUE);

    LayoutColumns();
}

// Paints radio buttons and check boxes in dark mode. The classic/themed
// controls draw the label in black whenever they hold keyboard focus, which is
// unreadable on our dark background, so the whole control is drawn here while
// the native procedure still handles clicks, keyboard and state.
LRESULT CALLBACK ThemeButtonSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR, DWORD_PTR) {
    if (!theme::IsDark()) return DefSubclassProc(hwnd, msg, wParam, lParam);

    if (msg == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, theme::WindowBrush());
        return 1;
    }

    if (msg != WM_PAINT) return DefSubclassProc(hwnd, msg, wParam, lParam);

    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, theme::WindowBrush());

    const LONG kind = GetWindowLongW(hwnd, GWL_STYLE) & 0x0F;
    const bool isRadio = kind == BS_AUTORADIOBUTTON || kind == BS_RADIOBUTTON;
    const bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool focused = GetFocus() == hwnd;

    const int height = rc.bottom - rc.top;
    int glyph = (height * 3) / 4;
    const int maxGlyph = ScaleForDpi(hwnd, 16);
    const int minGlyph = ScaleForDpi(hwnd, 11);
    if (glyph > maxGlyph) glyph = maxGlyph;
    if (glyph < minGlyph) glyph = minGlyph;
    const int gy = (height - glyph) / 2;
    const int gx = 1;

    const COLORREF outline = focused ? theme::SelectionBkColor() : theme::TextColor();
    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, outline);
    HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, hollow);

    if (isRadio) {
        Ellipse(hdc, gx, gy, gx + glyph, gy + glyph);
        if (checked) {
            HBRUSH dot = CreateSolidBrush(theme::SelectionBkColor());
            SelectObject(hdc, dot);
            const int inset = glyph / 4;
            Ellipse(hdc, gx + inset, gy + inset, gx + glyph - inset, gy + glyph - inset);
            SelectObject(hdc, hollow);
            DeleteObject(dot);
        }
    } else {
        Rectangle(hdc, gx, gy, gx + glyph, gy + glyph);
        if (checked) {
            HPEN tick = CreatePen(PS_SOLID, 2, theme::SelectionBkColor());
            SelectObject(hdc, tick);
            MoveToEx(hdc, gx + glyph / 4, gy + glyph / 2, nullptr);
            LineTo(hdc, gx + glyph / 2, gy + (3 * glyph) / 4);
            LineTo(hdc, gx + (3 * glyph) / 4, gy + glyph / 4);
            SelectObject(hdc, pen);
            DeleteObject(tick);
        }
    }

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);

    wchar_t text[256] = {0};
    GetWindowTextW(hwnd, text, 256);
    RECT textRect = rc;
    textRect.left = gx + glyph + ScaleForDpi(hwnd, 7);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, theme::TextColor());
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = SelectObject(
        hdc, font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    DrawTextW(hdc, text, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);

    if (focused) {
        // Native buttons draw the focus rectangle around the label only. A box
        // around the whole (much wider) control looks like an edit field.
        RECT focus = textRect;
        DrawTextW(hdc, text, -1, &focus, DT_LEFT | DT_SINGLELINE | DT_CALCRECT);
        const int textHeight = focus.bottom - focus.top;
        focus.top = (rc.bottom - rc.top - textHeight) / 2 - 1;
        focus.bottom = focus.top + textHeight + 1;
        focus.left -= 2;
        focus.right += 1;
        DrawFocusRect(hdc, &focus);
    }

    EndPaint(hwnd, &ps);
    return 0;
}

// Owns the painting of group boxes in dark mode. The themed group box draws
// its caption in black and lets the frame run through the text, which is
// invisible on a dark background, so the frame and caption are drawn here.
LRESULT CALLBACK GroupBoxSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR) {
    if (!theme::IsDark()) return DefSubclassProc(hwnd, msg, wParam, lParam);

    if (msg == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, theme::WindowBrush());
        return 1;
    }

    if (msg != WM_PAINT) return DefSubclassProc(hwnd, msg, wParam, lParam);

    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, theme::WindowBrush());

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = SelectObject(
        hdc, font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));

    const int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    const int textLeft = MulDiv(8, dpi, 96);

    wchar_t text[256] = {0};
    GetWindowTextW(hwnd, text, 256);
    SIZE extent{};
    GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &extent);

    // Frame runs along the vertical middle of the caption, as native group
    // boxes do, with the caption drawn opaquely over it.
    const int frameTop = extent.cy / 2;
    const int radius = MulDiv(6, dpi, 96);
    HPEN pen = CreatePen(PS_SOLID, 1, theme::BorderColor());
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, frameTop, rc.right - 1, rc.bottom - 1, radius, radius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    RECT textRect{textLeft, 0, textLeft + extent.cx + 1, extent.cy};
    FillRect(hdc, &textRect, theme::WindowBrush());

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, theme::TextColor());
    DrawTextW(hdc, text, -1, &textRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

    SelectObject(hdc, oldFont);
    EndPaint(hwnd, &ps);
    return 0;
}

// Owns the painting of the ListView's header so its colours are deterministic.
// SetWindowTheme("DarkMode_Explorer") on the header is unreliable (it can fall
// back to the light drawing depending on the control's state), and the header
// does not forward its custom-draw notification to the main window, so the
// control is painted directly here.
LRESULT CALLBACK HeaderSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                UINT_PTR, DWORD_PTR) {
    if (!theme::IsDark()) return DefSubclassProc(hwnd, msg, wParam, lParam);

    if (msg == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, theme::WindowBrush());
        return 1;
    }

    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &ps.rcPaint, theme::WindowBrush());

        HGDIOBJ oldFont = SelectObject(
            hdc, g_fontHeader ? g_fontHeader
                              : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));

        const int count = Header_GetItemCount(hwnd);
        for (int i = 0; i < count; ++i) {
            RECT item{};
            if (!Header_GetItemRect(hwnd, i, &item)) continue;

            wchar_t text[256] = {0};
            HDITEMW info{};
            info.mask = HDI_TEXT;
            info.pszText = text;
            info.cchTextMax = 256;
            Header_GetItem(hwnd, i, &info);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, theme::TextColor());
            RECT tr = item;
            tr.left += ScaleForDpi(hwnd, 5);
            tr.right -= ScaleForDpi(hwnd, 4);
            DrawTextW(hdc, text, -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            RECT sep = item;
            sep.left = sep.right - 1;
            HBRUSH sepBrush = CreateSolidBrush(theme::SeparatorColor());
            FillRect(hdc, &sep, sepBrush);
            DeleteObject(sepBrush);
        }
        SelectObject(hdc, oldFont);
        EndPaint(hwnd, &ps);
        return 0;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------- dark message box
// Native MessageBoxW dialogs stay partly light in dark mode no matter how they
// are themed (a CBT-hook build proved the whole theming pipeline ran and the
// dialog still had white parts), so prompts use our own dialog resource
// (IDD_MSGBOX): palette and buttons are painted entirely by us.

enum MessageBoxKind { MsgOk = 0, MsgYesNo = 1, MsgYesNoCancel = 2 };

// Message spec handed to the dialog proc through DialogBoxParamW's lParam;
// it lives on the caller's stack for the whole modal run.
struct MsgSpec {
    const wchar_t* caption;
    const wchar_t* text;
    MessageBoxKind kind;
};

MessageBoxKind KindOf(unsigned int type) {
    if (type & MB_YESNOCANCEL) return MsgYesNoCancel;
    if (type & MB_YESNO) return MsgYesNo;
    return MsgOk;
}

INT_PTR CALLBACK MsgBoxProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        const auto* spec = reinterpret_cast<const MsgSpec*>(lParam);

        SetWindowTextW(dlg, spec->caption);
        theme::ApplyWindow(dlg);   // dark title bar, before the dialog is shown

        RECT win0{};
        GetWindowRect(dlg, &win0);
        RECT cli0{};
        GetClientRect(dlg, &cli0);
        const int frameH = (win0.bottom - win0.top) - (cli0.bottom - cli0.top);
        const int pad = ScaleForDpi(dlg, 14);
        const int btnH = ScaleForDpi(dlg, 20);
        const int btnGap = ScaleForDpi(dlg, 8);

        // Text: set first, then measure the real wrapped height and fit the
        // window around it.
        const HWND text = GetDlgItem(dlg, IDC_MSG_TEXT);
        SendMessageW(text, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUI), TRUE);
        SetWindowTextW(text, spec->text);
        HDC dc = GetDC(text);
        HGDIOBJ oldFont = SelectObject(dc, g_fontUI);
        RECT measure{0, 0, (cli0.right - cli0.left) - 2 * pad, 0};
        DrawTextW(dc, spec->text, -1, &measure,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        ReleaseDC(text, dc);
        MoveWindow(text, pad, pad, (cli0.right - cli0.left) - 2 * pad,
                   measure.bottom + ScaleForDpi(dlg, 4), TRUE);

        RECT win{};
        GetWindowRect(dlg, &win);
        const int newClientH = pad + measure.bottom + btnGap + btnH + pad;
        SetWindowPos(dlg, nullptr, 0, 0, win.right - win.left,
                     newClientH + frameH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        RECT cli{};
        GetClientRect(dlg, &cli);
        const int rowY = (cli.bottom - cli.top) - pad - btnH;

        // Buttons, right-aligned: [0]=leftmost ... [count-1]=rightmost.
        struct BtnSpec { const wchar_t* label; int id; bool def; };
        static const BtnSpec kinds3[] = {  // Yes/No/Cancel: cancel is default
            {L"Cancel and close",     IDYES,    false},
            {L"Close, let it finish", IDNO,     false},
            {L"Keep waiting",         IDCANCEL, true},
        };
        static const BtnSpec kinds2[] = {  // Yes/No: No is default
            {L"No",  IDNO,  true},
            {L"Yes", IDYES, false},
        };
        static const BtnSpec kinds1[] = {
            {L"OK", IDOK, true},
        };
        // spec->kind is already the MessageBoxKind chosen by DarkMessageBox;
        // running KindOf() on it again misreads the enum as a flag mask
        // (MsgYesNo == 1 shares a bit with MB_YESNOCANCEL) and shows the
        // exit dialog's buttons on every prompt.
        const MessageBoxKind kind = spec->kind;
        const BtnSpec* defs = kind == MsgYesNoCancel ? kinds3
                            : kind == MsgYesNo ? kinds2
                                               : kinds1;
        const int count = kind == MsgYesNoCancel ? 3 : kind == MsgYesNo ? 2 : 1;
        const int btnIds[3] = {IDC_MSG_BTN1, IDC_MSG_BTN2, IDC_MSG_BTN3};

        int x = (cli.right - cli.left) - pad;
        int defBtnId = IDCANCEL;
        for (int i = count - 1; i >= 0; --i) {
            const HWND btn = GetDlgItem(dlg, btnIds[i]);
            dc = GetDC(btn);
            oldFont = SelectObject(dc, g_fontUI);
            SIZE extent{};
            GetTextExtentPoint32W(dc, defs[i].label,
                                  static_cast<int>(wcslen(defs[i].label)), &extent);
            SelectObject(dc, oldFont);
            ReleaseDC(btn, dc);

            const int w = std::max(static_cast<int>(extent.cx) + ScaleForDpi(dlg, 28),
                                   ScaleForDpi(dlg, 80));
            x -= w;
            MoveWindow(btn, x, rowY, w, btnH, TRUE);
            x -= btnGap;

            SetWindowTextW(btn, defs[i].label);
            SetWindowLongPtrW(btn, GWLP_ID, static_cast<LONG_PTR>(defs[i].id));
            if (defs[i].def) defBtnId = defs[i].id;

            auto* visual = new ButtonVisual();
            visual->primary = (defs[i].id == IDOK || defs[i].id == IDYES);
            SetWindowSubclass(btn, ModernButtonSubclass,
                              static_cast<UINT_PTR>(btnIds[i]),
                              reinterpret_cast<DWORD_PTR>(visual));
            SendMessageW(btn, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUI), TRUE);
        }
        for (int i = count; i < 3; ++i) {
            ShowWindow(GetDlgItem(dlg, btnIds[i]), SW_HIDE);
        }
        SendMessageW(dlg, DM_SETDEFID, static_cast<WPARAM>(defBtnId), 0);

        // Centre on the owner.
        RECT dlgRect{};
        GetWindowRect(dlg, &dlgRect);
        RECT ownerRect{};
        GetWindowRect(g_hwnd ? g_hwnd : dlg, &ownerRect);
        const int cx = ownerRect.left + ((ownerRect.right - ownerRect.left) -
                                         (dlgRect.right - dlgRect.left)) / 2;
        const int cy = ownerRect.top + ((ownerRect.bottom - ownerRect.top) -
                                        (dlgRect.bottom - dlgRect.top)) / 2;
        SetWindowPos(dlg, nullptr, cx, cy, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        // Keyboard focus on the default button.
        PostMessageW(dlg, WM_NEXTDLGCTL,
                     reinterpret_cast<WPARAM>(GetDlgItem(dlg, defBtnId)), TRUE);
        return TRUE;
    }

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(dlg, &rc);
        FillRect(hdc, &rc, theme::WindowBrush());
        return 1;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, theme::TextColor());
        SetBkColor(hdc, theme::WindowColor());
        return reinterpret_cast<LRESULT>(theme::WindowBrush());
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDYES ||
            LOWORD(wParam) == IDNO || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dlg, LOWORD(wParam));
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;

    default:
        break;
    }
    return FALSE;
}

int DarkMessageBox(HWND owner, const wchar_t* text, const wchar_t* caption,
                   unsigned int type) {
    MsgSpec spec{caption, text, KindOf(type)};
    const INT_PTR result = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_MSGBOX),
                                           owner, MsgBoxProc,
                                           reinterpret_cast<LPARAM>(&spec));
    return result == -1 ? IDCANCEL : static_cast<int>(result);
}

// ---------------------------------------------------------------- messages
LRESULT OnNotify(LPARAM lParam) {
    const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
    if (!header || header->idFrom != IDC_LIST) return 0;

    if (header->code == LVN_ITEMCHANGED && !g_filling) {
        const NMLISTVIEW* change = reinterpret_cast<const NMLISTVIEW*>(lParam);
        const UINT oldState = change->uOldState & LVIS_STATEIMAGEMASK;
        const UINT newState = change->uNewState & LVIS_STATEIMAGEMASK;
        if (oldState != newState) {
            UpdateStatusText();
            RefreshButtons();
        }
        return 0;
    }

    if (header->code == NM_CUSTOMDRAW) {
        auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
        switch (draw->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;

        case CDDS_ITEMPREPAINT:
            if (theme::IsDark()) {
                const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
                draw->clrText = selected ? theme::SelectionTextColor() : theme::TextColor();
                draw->clrTextBk = selected ? theme::SelectionBkColor() : theme::WindowColor();
                return CDRF_NEWFONT;
            }
            break;

        case CDDS_ITEMPOSTPAINT: {
            const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
            if (!selected) {
                RECT line = draw->nmcd.rc;
                line.top = line.bottom - 1;
                HBRUSH brush = CreateSolidBrush(theme::SeparatorColor());
                FillRect(draw->nmcd.hdc, &line, brush);
                DeleteObject(brush);
            }
            break;
        }

        default:
            break;
        }
        return CDRF_DODEFAULT;
    }

    return 0;
}

LRESULT OnCommand(WPARAM wParam) {
    switch (LOWORD(wParam)) {
        case IDC_BTN_CHECK:    StartCheck();       break;
        case IDC_BTN_UPDATE:   StartSelected();    break;
        case IDC_BTN_ALL:      StartAll();         break;
        case IDC_BTN_SEL_ALL:  SetAllChecks(TRUE); break;
        case IDC_BTN_SEL_NONE: SetAllChecks(FALSE); break;
        case IDC_BTN_SETTINGS: ShowSettings();     break;
        default: return 0;
    }
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        theme::ApplyWindow(hwnd);
        CreateControls(hwnd);
        Layout(hwnd);
        PostMessageW(hwnd, WM_APP_STARTCHECK, 0, 0);
        return 0;

    case WM_SIZE:
        Layout(hwnd);
        return 0;

    case WM_DPICHANGED: {
        const RECT* prc = reinterpret_cast<const RECT*>(lParam);
        if (prc) {
            SetWindowPos(hwnd, nullptr,
                         prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        CreateFonts(hwnd);
        ApplyFonts();
        UpdateListRowHeight(hwnd);
        Layout(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, theme::WindowBrush());
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        FillRect(hdc, &ps.rcPaint, theme::WindowBrush());
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = ScaleForDpi(hwnd, 860);
        info->ptMinTrackSize.y = ScaleForDpi(hwnd, 540);
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (g_settings.theme == settings::ThemeSystem && lParam &&
            wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            ApplyThemeLive();
        }
        return 0;

    case WM_APP_STARTCHECK:
        StartCheck();
        return 0;

    case WM_APP_LOGTEXT: {
        wchar_t* text = reinterpret_cast<wchar_t*>(lParam);
        if (text) {
            AppendLog(text);
            delete[] text;
        }
        return 0;
    }

    case WM_APP_STATUS: {
        wchar_t* text = reinterpret_cast<wchar_t*>(lParam);
        if (text) {
            SetWindowTextW(g_status, text);
            delete[] text;
        }
        return 0;
    }

    case WM_APP_UPDATES: {
        auto* list = reinterpret_cast<std::vector<Package>*>(lParam);
        if (list) {
            g_packages = std::move(*list);
            delete list;
            FillList();
        }
        return 0;
    }

    case WM_APP_FINISHED: {
        wchar_t* note = reinterpret_cast<wchar_t*>(lParam);
        g_busy.store(0, std::memory_order_release);
        RefreshButtons();
        if (note) {
            SetWindowTextW(g_status, note);
            delete[] note;
        } else {
            UpdateStatusText();
        }
        return 0;
    }

    case WM_NOTIFY:
        return OnNotify(lParam);

    case WM_COMMAND:
        return OnCommand(wParam);

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        if (control == g_log) {
            SetTextColor(hdc, theme::FieldTextColor());
            SetBkColor(hdc, theme::FieldColor());
            return reinterpret_cast<LRESULT>(theme::FieldBrush());
        }
        SetTextColor(hdc, theme::TextColor());
        SetBkColor(hdc, theme::WindowColor());
        return reinterpret_cast<LRESULT>(theme::WindowBrush());
    }

    case WM_CLOSE: {
        if (g_busy.load()) {
            const int choice = DarkMessageBox(
                hwnd,
                L"An update is still running.\n\n"
                L"Yes - cancel winget and close now. A partially installed\n"
                L"package may need reinstallation.\n"
                L"No - close and let winget finish in the background.\n"
                L"Cancel - keep the window open.",
                kTitle, MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
            if (choice == IDCANCEL) return 0;
            if (choice == IDYES && g_job) TerminateJobObject(g_job, 1);
        }
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
        g_alive.store(0, std::memory_order_release);
        if (g_job) {
            CloseHandle(g_job);  // no kill-on-close: winget may keep running
            g_job = nullptr;
        }
        DeleteFonts();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void LogStartupBanner() {
    PostLog(L"Winget Update Manager 1.0.2");
    PostLog(theme::IsDark() ? L"Theme: dark." : L"Theme: light.");
    if (g_elevated) {
        PostLog(L"Running with administrator rights.");
    } else {
        PostLog(L"Running as a standard user. If an installer needs administrator");
        PostLog(L"rights, enable \"Always run as administrator\" in Settings.");
    }
}

}  // namespace

// ---------------------------------------------------------------- entry point
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    g_inst = instance;
    g_settings = settings::Load();
    g_elevated = IsElevated();

    // Honour the stored "always run as administrator" preference.
    if (g_settings.runAsAdmin && !g_elevated) {
        if (RelaunchElevated()) return 0;
    }

    theme::Init(EffectiveDark());

    g_iconLarge = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDR_APP_ICON),
                                                IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    g_iconSmall = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDR_APP_ICON),
                                                IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), 0));

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // WM_ERASEBKGND paints the themed background
    wc.lpszClassName = L"WingetUpdateManagerWnd";
    wc.hIcon = g_iconLarge ? g_iconLarge : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = g_iconSmall ? g_iconSmall : wc.hIcon;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Could not register the window class.",
                    kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }

    const int width = ScaleForDpi(nullptr, 940);
    const int height = ScaleForDpi(nullptr, 640);
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, kTitle,
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, width, height,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Could not create the main window.",
                    kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);

    LogStartupBanner();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}