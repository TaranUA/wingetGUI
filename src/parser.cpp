// Parsing of the human-readable table produced by `winget upgrade`.
//
// winget has no machine-readable output for upgrades, so we rely on the
// fixed-width table. Column start offsets come from the header row; each data
// row is split by display columns, matching WinGet's table padding.

#include "parser.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <set>

namespace winget {
namespace {

std::wstring TrimW(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' ||
                     s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(b, e - b);
}

std::wstring LowerW(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool IsAllDashes(const std::string& s) {
    bool dash = false;
    for (char c : s) {
        if (c != '-' && c != ' ' && c != '\t') return false;
        if (c == '-') dash = true;
    }
    return dash;
}

// Windows 10 1903+ supplies ICU. WinGet counts one column per grapheme,
// or two when its first code point is East Asian wide or fullwidth.
struct IcuColumns {
    using Open = void* (*)(int, const char*, const wchar_t*, int, int*);
    using Next = int (*)(void*);
    using Close = void (*)(void*);
    using Property = int (*)(int, int);

    HMODULE module = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Open open = Resolve<Open>("ubrk_open");
    Next next = Resolve<Next>("ubrk_next");
    Close close = Resolve<Close>("ubrk_close");
    Property property = Resolve<Property>("u_getIntPropertyValue");

    template <typename Fn>
    Fn Resolve(const char* name) const {
        return module ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name))) : nullptr;
    }
};

std::vector<size_t> ColumnOffsets(const std::wstring& row) {
    static const IcuColumns icu;
    constexpr int characterBreak = 0;     // UBRK_CHARACTER
    constexpr int eastAsianWidth = 0x1004; // UCHAR_EAST_ASIAN_WIDTH
    constexpr int fullwidth = 3, wide = 5;
    const int size = static_cast<int>(row.size());
    int status = 0;
    void* breaks = icu.open && icu.next && icu.close && icu.property ?
        icu.open(characterBreak, "root", row.data(), size, &status) : nullptr;
    std::vector<size_t> offsets;
    offsets.reserve(row.size() + 1);
    for (int start = 0; start < size;) {
        int end = breaks ? icu.next(breaks) : start + 1;
        int cp = row[start];
        if (IS_HIGH_SURROGATE(cp) && start + 1 < size && IS_LOW_SURROGATE(row[start + 1])) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + row[start + 1] - 0xDC00;
            if (!breaks) ++end;
        }
        if (end <= start) break;
        offsets.push_back(static_cast<size_t>(start));
        if (breaks) {
            const int width = icu.property(cp, eastAsianWidth);
            if (width == wide || width == fullwidth) offsets.push_back(static_cast<size_t>(start));
        } else {
            WORD type = 0;
            if (GetStringTypeW(CT_CTYPE3, &row[start], 1, &type) && (type & C3_FULLWIDTH)) {
                offsets.push_back(static_cast<size_t>(start));
            }
        }
        start = end;
    }
    if (breaks) icu.close(breaks);
    offsets.push_back(row.size());
    return offsets;
}

// Finds an ASCII column heading, requiring space (or string edge) boundaries.
int FindToken(const std::string& s, const char* tok) {
    const size_t tl = std::strlen(tok);
    if (tl == 0 || s.size() < tl) return -1;
    for (size_t p = 0; p + tl <= s.size(); ++p) {
        if (p != 0 && s[p - 1] != ' ') continue;
        if (s.compare(p, tl, tok) != 0) continue;
        if (p + tl != s.size() && s[p + tl] != ' ') continue;
        return static_cast<int>(p);
    }
    return -1;
}

// True when the next non-blank line of `text` starting at `pos` is the dash
// rule that follows a real winget table header.
bool NextLineIsDashes(const std::string& text, size_t pos) {
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        if (!line.empty()) return IsAllDashes(line);
        pos = end + 1;
    }
    return false;
}

}  // namespace

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();

    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int need = MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) {  // not valid UTF-8: fall back to the ANSI code page
        cp = CP_ACP;
        flags = 0;
        need = MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (need <= 0) return std::wstring();
    }

    std::wstring out(static_cast<size_t>(need), L'\0');
    const int got = MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()),
                                        &out[0], need);
    if (got <= 0) return std::wstring();
    out.resize(static_cast<size_t>(got));
    return out;
}

std::string StripAnsi(const std::string& in) {
    std::string out;
    out.reserve(in.size());

    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);

        if (c == 0x1B) {  // ESC: drop the whole escape sequence
            if (i + 1 >= in.size()) break;
            const char kind = in[i + 1];
            if (kind == '[') {
                i += 2;
                while (i < in.size()) {
                    const unsigned char d = static_cast<unsigned char>(in[i]);
                    if (d >= 0x40 && d <= 0x7E) break;  // final byte
                    ++i;
                }
            } else if (kind == ']' || kind == 'P' || kind == '^' || kind == '_') {
                i += 2;
                while (i < in.size()) {
                    if (static_cast<unsigned char>(in[i]) == 0x07) break;        // BEL
                    if (static_cast<unsigned char>(in[i]) == 0x1B &&
                        i + 1 < in.size() && in[i + 1] == '\\') { ++i; break; }  // ST
                    ++i;
                }
            } else {
                ++i;
            }
            continue;
        }

        if (c == '\t') { out.push_back('\t'); continue; }
        if (c < 0x20) continue;  // strip stray control characters
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::wstring EscapeArg(const std::wstring& s) {
    if (s.empty()) return L"\"\"";

    std::wstring out;
    out.push_back(L'"');

    int backslashes = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            // 2N + 1 backslashes followed by a quotation mark emit N backslashes
            // followed by a literal quotation mark.
            out.append(static_cast<size_t>(backslashes * 2 + 1), L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(static_cast<size_t>(backslashes), L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    // 2N backslashes before the closing quote emit N backslashes.
    out.append(static_cast<size_t>(backslashes * 2), L'\\');
    out.push_back(L'"');
    return out;
}

std::vector<Package> ParseUpgradeOutput(const std::string& text) {
    std::vector<Package> out;
    std::set<std::pair<std::wstring, std::wstring>> seen;

    int colName = -1, colId = -1, colVer = -1, colAvail = -1, colSrc = -1;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        if (line.empty() || IsAllDashes(line)) continue;

        const int n = FindToken(line, "Name");
        const int i = FindToken(line, "Id");
        const int v = FindToken(line, "Version");
        const int a = FindToken(line, "Available");
        const int s = FindToken(line, "Source");

        // A genuine header is always followed by the dash rule. Data rows or
        // banner text that merely contain the heading words do not, so they
        // must not shift the column offsets.
        if (n >= 0 && i > n && a > i && NextLineIsDashes(text, pos)) {  // header row
            colName = n;
            colId = i;
            colAvail = a;
            colVer = (v > i && v < a) ? v : -1;
            colSrc = (s > a) ? s : -1;
            continue;
        }

        if (colName < 0) continue;  // no table seen yet

        const std::wstring w = Utf8ToWide(line);
        const auto offsets = ColumnOffsets(w);
        const int columns = static_cast<int>(offsets.size()) - 1;
        if (columns <= colAvail) continue;

        const int idEnd = (colVer > 0) ? colVer : colAvail;

        auto slice = [&](int from, int to) -> std::wstring {
            to = (std::min)(to, columns);
            if (from >= to) return std::wstring();
            return TrimW(w.substr(offsets[from], offsets[to] - offsets[from]));
        };

        Package p;
        p.name      = slice(colName, colId);
        p.id        = slice(colId, idEnd);
        p.current   = (colVer > 0) ? slice(colVer, colAvail) : std::wstring();
        p.available = slice(colAvail, colSrc > 0 ? colSrc : columns);
        p.source    = (colSrc > 0) ? slice(colSrc, columns)
                                   : std::wstring();

        // Reject banners, summaries and the trailing explanatory paragraph:
        // a genuine row has a compact, space-free id.
        if (p.name.empty() || p.id.size() < 2) continue;
        if (p.id.find_first_of(L" \t\u2026") != std::wstring::npos ||
            p.source.find(L'\u2026') != std::wstring::npos) continue;
        if (p.available.empty()) continue;

        if (!seen.emplace(LowerW(p.id), LowerW(p.source)).second) continue;
        out.push_back(std::move(p));
    }

    return out;
}

bool ReportsNoUpdates(const std::string& text) {
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        const auto line = text.substr(start, end - start);
        if (line == "No installed package found matching input criteria." ||
            line == "No applicable update found." || line == "No available upgrade found." ||
            line == "No installed package has an update available." ||
            line == "No applicable updates found.") return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

}  // namespace winget
