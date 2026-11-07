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

std::wstring ReadPath() {
    const std::wstring portable = PortablePath();
    if (FileExists(portable)) return portable;
    const std::wstring fallback = LocalAppDataPath();
    if (FileExists(fallback)) return fallback;
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

    std::wstring path = PortablePath();

    auto write = [&](const std::wstring& file) -> bool {
        if (file.empty()) return false;
        if (!WritePrivateProfileStringW(L"ui", L"theme", theme, file.c_str())) return false;
        if (!WritePrivateProfileStringW(L"ui", L"run_as_admin",
                                        v.runAsAdmin ? L"1" : L"0", file.c_str())) {
            return false;
        }
        return true;
    };

    if (write(path)) return;
    write(LocalAppDataPath());  // executable directory is read-only
}

}  // namespace settings