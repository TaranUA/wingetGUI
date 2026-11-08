#include "settings.h"

#include <windows.h>

#include <string>

namespace settings {
namespace {

std::wstring DirectoryOfExecutable() {
    wchar_t path[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return std::wstring();
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

std::wstring LocalAppDataPath() {
    wchar_t buffer[MAX_PATH] = {0};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buffer)
                                               : DirectoryOfExecutable();
    if (dir.empty()) return L"settings.ini";
    dir += L"\\WingetGUI";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

std::wstring PortablePath() {
    const std::wstring dir = DirectoryOfExecutable();
    if (dir.empty()) return std::wstring();
    return dir + L"\\WingetGUI.ini";
}

bool FileExists(const std::wstring& path) {
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsWritable(const std::wstring& path) {
    // CreateFileW with GENERIC_WRITE and an immediate close: cheap probe that
    // answers the only question Load()/Save() care about - can we write here?
    if (path.empty()) return false;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    CloseHandle(handle);
    return true;
}

std::wstring ReadPath() {
    const std::wstring portable = PortablePath();
    // Mirror Save()'s choice: only use the copy next to the executable when it
    // is actually writable. A stale or read-only file there (typical when the
    // app was installed into Program Files) would otherwise shadow the
    // settings written to %LOCALAPPDATA%, and the caller's settings would
    // silently revert - visibly as a dropped "always run as administrator".
    if (FileExists(portable) && IsWritable(portable)) return portable;
    const std::wstring fallback = LocalAppDataPath();
    if (FileExists(fallback) && IsWritable(fallback)) return fallback;
    return portable;
}

}  // namespace

Values Load() {
    Values v;

    const std::wstring path = ReadPath();
    if (path.empty()) return v;

    v.theme = static_cast<int>(GetPrivateProfileIntW(L"ui", L"theme", ThemeSystem,
                                                     path.c_str()));
    if (v.theme < ThemeSystem || v.theme > ThemeDark) v.theme = ThemeSystem;

    v.runAsAdmin = GetPrivateProfileIntW(L"ui", L"run_as_admin", 0, path.c_str()) != 0;
    return v;
}

void Save(const Values& v) {
    const wchar_t* theme = L"0";
    if (v.theme == ThemeLight) theme = L"1";
    else if (v.theme == ThemeDark) theme = L"2";

    // Both keys go to a temp file first, then the temp file replaces the real
    // one atomically. A crash between the two key writes would otherwise leave
    // the ini half-updated - e.g. a new theme with the old admin flag.
    auto write = [&](const std::wstring& file) -> bool {
        if (file.empty()) return false;
        const size_t slash = file.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return false;
        const std::wstring tmp = file.substr(0, slash + 1) + L"settings.tmp";

        if (!CopyFileW(file.c_str(), tmp.c_str(), FALSE)) {
            // The target may not exist yet; that is fine - the temp file is
            // built from scratch in that case.
            if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES &&
                GetLastError() == ERROR_FILE_NOT_FOUND) {
                if (!WritePrivateProfileStringW(L"ui", L"theme", theme, tmp.c_str())) {
                    return false;
                }
                if (!WritePrivateProfileStringW(L"ui", L"run_as_admin",
                                                v.runAsAdmin ? L"1" : L"0",
                                                tmp.c_str())) {
                    return false;
                }
            } else {
                return false;
            }
        } else {
            if (!WritePrivateProfileStringW(L"ui", L"theme", theme, tmp.c_str()) ||
                !WritePrivateProfileStringW(L"ui", L"run_as_admin",
                                            v.runAsAdmin ? L"1" : L"0",
                                            tmp.c_str())) {
                DeleteFileW(tmp.c_str());
                return false;
            }
        }

        if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    };

    const std::wstring path = PortablePath();
    if (write(path)) return;
    write(LocalAppDataPath());  // executable directory is read-only
}

}  // namespace settings