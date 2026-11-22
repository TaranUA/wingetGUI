// Exercise the production Win32 worker helpers using this executable as the child.
#include "../src/main.cpp"

#include <cstdio>

namespace {

int failures = 0;

void Check(bool passed, const char* description) {
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", description);
    if (!passed) ++failures;
}

struct ChildRun {
    std::wstring executable;
    std::wstring readyEvent;
    DWORD exitCode = 0;
    DWORD error = 0;
    bool ran = false;
};

DWORD WINAPI RunSlowChild(void* argument) {
    auto& run = *static_cast<ChildRun*>(argument);
    OutputParser sink;
    run.ran = RunProcess(run.executable, L"--slow-child " + winget::EscapeArg(run.readyEvent),
                         sink, run.exitCode, run.error);
    return 0;
}

void TestShutdown(const std::wstring& executable, bool cancel) {
    g_alive.store(1);
    ChildRun run{executable, L"Local\\WingetGUI-runtime-" + NumW(GetCurrentProcessId())};
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, run.readyEvent.c_str());
    HANDLE thread = ready ? CreateThread(nullptr, 0, RunSlowChild, &run, 0, nullptr) : nullptr;
    if (!thread) {
        Check(false, "create child worker");
        if (ready) CloseHandle(ready);
        StopLaunching(false);
        return;
    }

    const HANDLE signals[] = {ready, thread};
    Check(WaitForMultipleObjects(2, signals, FALSE, 5000) == WAIT_OBJECT_0, "child signals it has started");
    Check(StopLaunching(cancel), cancel ? "cancel active command" : "release active command");
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(ready);
    Check(run.ran && run.exitCode == (cancel ? ERROR_CANCELLED : 42),
          cancel ? "child exits with cancellation status" : "released child finishes normally");

    OutputParser sink;
    Check(!RunProcess(executable, L"--child", sink, run.exitCode, run.error) && run.error == ERROR_CANCELLED,
          "shutdown prevents a new child from starting");
}

void TestSettings(const std::wstring& executable) {
    const std::wstring directory = executable.substr(0, executable.find_last_of(L"\\/"));
    SetEnvironmentVariableW(L"LOCALAPPDATA", directory.c_str());
    Check(settings::Save({settings::ThemeLight, false}), "save portable settings");
    const std::wstring portable = directory + L"\\WingetGUI.ini";
    HANDLE held = CreateFileW(portable.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "hold portable INI open without delete sharing");
    Check(settings::Save({settings::ThemeDark, true}), "save fallback when portable INI cannot be replaced");
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    auto values = settings::Load();
    Check(values.theme == settings::ThemeDark && values.runAsAdmin, "load fallback after portable file is unlocked");
    Check(GetPrivateProfileIntW(L"ui", L"theme", 0, portable.c_str()) == settings::ThemeLight,
          "failed replacement preserved the portable INI");
    Check(settings::Save({settings::ThemeSystem, false}), "save back to the selected fallback");
    values = settings::Load();
    Check(values.theme == settings::ThemeSystem && !values.runAsAdmin, "reload both updated values");
}

void TestCheckResult() {
    std::vector<Package> packages(1);
    packages.front().id = L"Vendor.Existing";
    std::wstring note;
    Check(!ReadCheckResult(0x8A150045, "Source could not be opened.\n", packages, note) &&
          packages.size() == 1 && packages.front().id == L"Vendor.Existing" &&
          note.find(L"0x8A150045") != std::wstring::npos, "failed check preserves existing rows and reports the exit code");
    Check(!ReadCheckResult(ERROR_SUCCESS, "Unrecognized output.\n", packages, note) && packages.size() == 1,
          "successful exit with unreadable output preserves existing rows");
    Check(ReadCheckResult(kUpdateNotApplicable, "", packages, note) && packages.empty(),
          "no applicable updates clears stale rows");
}

int TestLiveWinget() {
    const std::wstring executable = FindWinget();
    if (executable.empty()) {
        std::puts("Registered App Installer executable is unavailable.");
        StopLaunching(false);
        return 1;
    }
    std::string output;
    OutputParser sink;
    sink.collect = &output;
    DWORD exitCode = 0, error = 0;
    const bool ran = RunProcess(executable, L"--version", sink, exitCode, error);
    std::printf("%sWinGet launch: ran=%d exit=%lu error=%lu\n", output.c_str(), ran,
                static_cast<unsigned long>(exitCode), static_cast<unsigned long>(error));
    Check(ran && exitCode == 0 && !output.empty(), "launch registered WinGet");

    if (!failures) {
        g_hwnd = CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    }
    for (int attempt = 0; attempt < 2 && !failures; ++attempt) {
        std::wstring note;
        const bool checked = g_hwnd && DoCheck(note);
        if (!checked) std::printf("Live check status: %ls\n", note.c_str());
        MSG message{};
        bool received = false;
        int commands = 0;
        bool showedUsage = false;
        while (g_hwnd && PeekMessageW(&message, g_hwnd, WM_APP_LOGTEXT, WM_APP_STATUS, PM_REMOVE)) {
            if (message.message == WM_APP_UPDATES) {
                std::unique_ptr<std::vector<Package>> packages(reinterpret_cast<std::vector<Package>*>(message.lParam));
                std::printf("Live check parsed %zu update(s).\n", packages->size());
                received = true;
            } else {
                std::unique_ptr<wchar_t[]> text(reinterpret_cast<wchar_t*>(message.lParam));
                if (message.message == WM_APP_LOGTEXT && text) {
                    if (wcsncmp(text.get(), L"> winget upgrade ", 17) == 0) ++commands;
                    if (wcsstr(text.get(), L"usage: winget upgrade")) showedUsage = true;
                }
                if (!checked && message.message == WM_APP_LOGTEXT && text && wcsncmp(text.get(), L"[!] ", 4) == 0) {
                    std::printf("%ls\n", text.get());
                }
            }
        }
        Check(checked && received, "check and parse live WinGet updates without installing them");
        Check(commands == 1 && !showedUsage, "check logs one discovery command without printing WinGet help");
    }
    if (g_hwnd) DestroyWindow(g_hwnd);
    StopLaunching(false);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 2 && wcscmp(argv[1], L"--child") == 0) {
        std::puts("child output");
        return 42;
    }
    if (argc == 3 && wcscmp(argv[1], L"--slow-child") == 0) {
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]);
        if (!ready) return 1;
        SetEvent(ready);
        CloseHandle(ready);
        Sleep(1500);
        // Write more than the pipe can buffer without an active reader.
        for (int i = 0; i < 10000; ++i) {
            if (std::puts("child output after close") < 0) return 2;
        }
        if (std::fflush(stdout) != 0) return 2;
        return 42;
    }

    if (argc == 2 && wcscmp(argv[1], L"--live-winget") == 0) return TestLiveWinget();

    wchar_t executable[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return 1;
    std::string output;
    OutputParser sink;
    sink.collect = &output;
    DWORD exitCode = 0, error = 0;
    Check(RunProcess(executable, L"--child", sink, exitCode, error) && exitCode == 42,
          "capture child exit code");
    Check(output.find("child output\n") != std::string::npos, "capture child output");
    StopLaunching(false);

    TestShutdown(executable, true);
    TestShutdown(executable, false);
    TestSettings(executable);
    TestCheckResult();
    std::printf("\nRuntime checks: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
