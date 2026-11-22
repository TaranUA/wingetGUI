#include "settings.h"

#include <windows.h>

#include <string>

namespace settings {
namespace {

std::wstring DirectoryOfExecutable() {
    std::wstring dir(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, dir.data(), static_cast<DWORD>(dir.size()));
    if (!length || length >= dir.size()) return {};
    dir.resize(length);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

std::wstring LocalAppDataPath() {
    wchar_t buffer[MAX_PATH] = {0};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buffer)
                                               : DirectoryOfExecutable();
    if (dir.empty()) return {};
    dir += L"\\WingetGUI";
    return dir + L"\\settings.ini";
}

std::wstring PortablePath() {
    const std::wstring dir = DirectoryOfExecutable();
    if (dir.empty()) return std::wstring();
    return dir + L"\\WingetGUI.ini";
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ReadPath() {
    const std::wstring fallback = LocalAppDataPath();
    return FileExists(fallback) ? fallback : PortablePath();
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

bool Save(const Values& v) {
    const wchar_t* theme = L"0";
    if (v.theme == ThemeLight) theme = L"1";
    else if (v.theme == ThemeDark) theme = L"2";

    auto write = [&](const std::wstring& file, bool createDirectory) -> bool {
        const size_t slash = file.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return false;
        const std::wstring directory = file.substr(0, slash + 1);
        if (createDirectory && !CreateDirectoryW(directory.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) return false;

        // Reserve a unique file on the destination volume before writing.
        wchar_t temp[MAX_PATH];
        if (!GetTempFileNameW(directory.c_str(), L"wgu", 0, temp)) return false;
        const bool copied = CopyFileW(file.c_str(), temp, FALSE) || GetLastError() == ERROR_FILE_NOT_FOUND;
        const bool saved = copied &&
            WritePrivateProfileStringW(L"ui", L"theme", theme, temp) &&
            WritePrivateProfileStringW(L"ui", L"run_as_admin", v.runAsAdmin ? L"1" : L"0", temp) &&
            MoveFileExW(temp, file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!saved) {
            SetFileAttributesW(temp, FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(temp);
        }
        return saved;
    };

    // Once a fallback exists it takes precedence for both reads and writes.
    // This also preserves updates made when a portable INI could not be replaced.
    const std::wstring fallback = LocalAppDataPath();
    if (!FileExists(fallback) && write(PortablePath(), false)) return true;
    return write(fallback, true);
}

}  // namespace settings
