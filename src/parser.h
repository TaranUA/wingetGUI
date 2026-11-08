#pragma once

#include <string>
#include <vector>

namespace winget {

struct Package {
    std::wstring name;
    std::wstring id;
    std::wstring current;
    std::wstring available;
    std::wstring source;
};

// Decodes UTF-8, falling back to the ANSI code page when the bytes are invalid.
std::wstring Utf8ToWide(const std::string& s);

// Removes ANSI escape sequences and stray control characters.
std::string StripAnsi(const std::string& s);

// Quotes and escapes an argument according to the standard Windows command-line
// parsing rules (CommandLineToArgvW / MSVCRT).
std::wstring EscapeArg(const std::wstring& s);

// Parses the fixed-width table printed by `winget upgrade`.
std::vector<Package> ParseUpgradeOutput(const std::string& text);

}  // namespace winget