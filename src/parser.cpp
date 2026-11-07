// Parsing of the human-readable table produced by `winget upgrade`.
//
// winget has no machine-readable output for upgrades, so we rely on the
// fixed-width table. Column start offsets come from the header row; each data
// row is decoded to UTF-16 before slicing so multi-byte names cannot shift the
// fields. Kept free of UI code so it can be unit-tested (tests/parse_test.cpp).

#include "parser.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <cstring>

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
    if (s.empty()) return false;
    for (char c : s) {
        if (c != '-' && c != ' ' && c != '\t') return false;
    }
    return true;
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

std::vector<Package> ParseUpgradeOutput(const std::string& text) {
    std::vector<Package> out;
    std::vector<std::wstring> seen;

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

        if (n >= 0 && i > n && a > i) {  // header row
            colName = n;
            colId = i;
            colAvail = a;
            colVer = (v > i && v < a) ? v : -1;
            colSrc = (s > a) ? s : -1;
            continue;
        }

        if (colName < 0) continue;  // no table seen yet

        const std::wstring w = Utf8ToWide(line);
        if (static_cast<int>(w.size()) <= colId) continue;

        const int idEnd = (colVer > 0) ? colVer : colAvail;

        auto slice = [&w](int from, int to) -> std::wstring {
            const int len = static_cast<int>(w.size());
            if (from < 0) from = 0;
            if (to > len) to = len;
            if (from >= to) return std::wstring();
            return TrimW(w.substr(static_cast<size_t>(from),
                                  static_cast<size_t>(to - from)));
        };

        Package p;
        p.name      = slice(colName, colId);
        p.id        = slice(colId, idEnd);
        p.current   = (colVer > 0) ? slice(colVer, colAvail) : std::wstring();
        p.available = slice(colAvail, colSrc > 0 ? colSrc : static_cast<int>(w.size()));
        p.source    = (colSrc > 0) ? slice(colSrc, static_cast<int>(w.size()))
                                   : std::wstring();

        // Reject banners, summaries and the trailing explanatory paragraph:
        // a genuine row has a compact, space-free id.
        if (p.name.empty() || p.id.size() < 2) continue;
        if (p.id.find(L' ') != std::wstring::npos) continue;
        if (p.available.empty()) continue;

        const std::wstring key = LowerW(p.id);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        out.push_back(std::move(p));
    }

    return out;
}

}  // namespace winget