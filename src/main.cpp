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
#include <shellapi.h>

#include <string>
#include <vector>
#include <algorithm>
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

volatile LONG g_busy = 0;    // 0 = idle, 1 = a worker is running
volatile LONG g_alive = 1;   // 0 once the window is gone
bool g_elevated = false;
bool g_filling = false;      // suppress notifications while populating

struct ButtonVisual {
    bool primary = false;
    bool hover = false;
    bool pressed = false;
};

void RefreshButtons();
void UpdateStatusText();
void ApplyThemeLive();
void PostLog(const std::wstring& text);

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
    if (!g_alive || !g_hwnd) return;
    wchar_t* p = DupStr(text);
    if (!PostMessageW(g_hwnd, WM_APP_LOGTEXT, 0, reinterpret_cast<LPARAM>(p))) delete[] p;
}

void PostStatus(const std::wstring& text) {
    if (!g_alive || !g_hwnd) return;
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
        if (g_alive && g_hwnd) {
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

    char buf[8192];
    DWORD n = 0;
    while (g_alive && ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
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
    if (!g_alive || !g_hwnd) {
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
            L"upgrade --id \"" + id + L"\" --exact --include-unknown "
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

    if (g_alive && g_hwnd) {
        PostMessageW(g_hwnd, WM_APP_FINISHED, 0,
                     note.empty() ? 0 : reinterpret_cast<LPARAM>(DupStr(note)));
    }

    delete work;
    return 0;
}

void BeginWork(WorkItem* work, const std::wstring& status) {
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        delete work;
        return;
    }
    SetWindowTextW(g_status, status.c_str());
    RefreshButtons();

    HANDLE thread = CreateThread(nullptr, 0, WorkerThread, work, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&g_busy, 0);
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
    if (g_busy || !g_status) return;

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
    const bool idle = (g_busy == 0);
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

    const int wId = 300, wCur = 110, wAvail = 110, wSrc = 100;
    int wName = total - (wId + wCur + wAvail + wSrc) - 24;
    if (wName < 150) wName = 150;

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
        MessageBoxW(g_hwnd, L"Select at least one package to update.",
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

    if (MessageBoxW(g_hwnd, question.c_str(), L"Update all",
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
void CreateFonts() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);

    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        LOGFONTW ui = metrics.lfMessageFont;
        ui.lfHeight = metrics.lfMessageFont.lfHeight - 1;  // a touch larger
        wcscpy(ui.lfFaceName, L"Segoe UI");
        g_fontUI = CreateFontIndirectW(&ui);

        LOGFONTW mono = metrics.lfMessageFont;
        mono.lfHeight = metrics.lfMessageFont.lfHeight - 1;
        mono.lfWeight = FW_NORMAL;
        wcscpy(mono.lfFaceName, L"Consolas");
        g_fontMono = CreateFontIndirectW(&mono);

        LOGFONTW header = ui;
        header.lfHeight = metrics.lfMessageFont.lfHeight - 2;
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

    const int radius = 10;  // corner ellipse diameter (5 px radius)
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
        InflateRect(&ring, -3, -3);
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

void CreateControls(HWND parent) {
    CreateFonts();

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
    {
        HDC dc = GetDC(g_list);
        HGDIOBJ oldFont = SelectObject(dc, g_fontUI);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, oldFont);
        ReleaseDC(g_list, dc);
        if (HIMAGELIST rows = ImageList_Create(1, tm.tmHeight + 12, ILC_COLOR32, 0, 1)) {
            ListView_SetImageList(g_list, rows, LVSIL_SMALL);
        }
    }

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

    HWND children[] = {g_status, g_list, g_log, g_btnCheck, g_btnUpdate, g_btnAll,
                       g_btnSelAll, g_btnSelNone, g_btnSettings};
    for (HWND child : children) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUI), TRUE);
    }
    SendMessageW(g_log, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontMono), TRUE);

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

    const int pad = 16;
    const int statusH = 22;
    const int btnH = 36;
    const int gap = 10;

    int logH = static_cast<int>(H * 0.26);
    if (logH < 96) logH = 96;
    if (logH > 280) logH = 280;

    int y = pad;
    MoveWindow(g_status, pad, y, W - 2 * pad, statusH, TRUE);
    y += statusH + 8;

    const int btnY = H - pad - logH - gap - btnH;
    int listH = btnY - gap - y;
    if (listH < 60) listH = 60;
    MoveWindow(g_list, pad, y, W - 2 * pad, listH, TRUE);

    int bx = pad;
    auto place = [&](HWND btn, int w) {
        MoveWindow(btn, bx, btnY, w, btnH, TRUE);
        bx += w + 8;
    };
    place(g_btnCheck, 150);
    place(g_btnUpdate, 145);
    place(g_btnAll, 110);
    bx += 18;
    place(g_btnSelAll, 100);
    place(g_btnSelNone, 110);

    MoveWindow(g_btnSettings, W - pad - 110, btnY, 110, btnH, TRUE);

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
    if (glyph > 16) glyph = 16;
    if (glyph < 11) glyph = 11;
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
    textRect.left = gx + glyph + 7;
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
            tr.left += 5;
            tr.right -= 4;
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
        info->ptMinTrackSize.x = 860;
        info->ptMinTrackSize.y = 540;
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
        InterlockedExchange(&g_busy, 0);
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

    case WM_CLOSE:
        if (g_busy) {
            if (MessageBoxW(hwnd, L"An update is still running. Close anyway?",
                            kTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return 0;
            }
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        InterlockedExchange(&g_alive, 0);
        if (g_fontUI && g_fontUI != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(g_fontUI);
        if (g_fontMono && g_fontMono != g_fontUI) DeleteObject(g_fontMono);
        if (g_fontHeader && g_fontHeader != g_fontUI) DeleteObject(g_fontHeader);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void LogStartupBanner() {
    PostLog(L"Winget Update Manager 1.0");
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

    const int width = 940;
    const int height = 640;
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