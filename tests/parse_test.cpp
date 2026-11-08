// Unit tests for the winget table parser (no UI, no winget required).
//
// Build & run with tests\run_tests.ps1, or:
//   g++ -std=c++17 -O2 -o build\parse_test.exe tests\parse_test.cpp src\parser.cpp

#include "../src/parser.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using winget::Package;
using winget::ParseUpgradeOutput;
using winget::StripAnsi;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

void CheckW(const std::wstring& got, const wchar_t* want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL: %s\n        got  [%ls]\n        want [%ls]\n",
                    what.c_str(), got.c_str(), want);
    }
}

void CheckSize(size_t got, size_t want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL: %s (got %zu rows, want %zu)\n", what.c_str(), got, want);
    }
}

struct Row {
    std::string name, id, version, available, source;
};

std::string Pad(const std::string& s, size_t width) {
    return s + std::string(width > s.size() ? width - s.size() : 0, ' ');
}

// Emits a table laid out exactly the way winget does: each column padded to the
// widest cell plus one separating space.
std::string MakeTable(const std::vector<Row>& rows, bool withSource = true) {
    size_t wName = 4, wId = 2, wVersion = 7, wAvailable = 9, wSource = 6;
    for (const Row& r : rows) {
        wName = std::max(wName, r.name.size());
        wId = std::max(wId, r.id.size());
        wVersion = std::max(wVersion, r.version.size());
        wAvailable = std::max(wAvailable, r.available.size());
        wSource = std::max(wSource, r.source.size());
    }
    wName += 1;
    wId += 1;
    wVersion += 1;
    wAvailable += 1;

    std::string out;
    out += Pad("Name", wName) + Pad("Id", wId) + Pad("Version", wVersion) +
           Pad("Available", wAvailable) + (withSource ? "Source" : "") + "\n";
    out += std::string(wName + wId + wVersion + wAvailable + (withSource ? 6 : 0), '-') + "\n";

    for (const Row& r : rows) {
        out += Pad(r.name, wName) + Pad(r.id, wId) + Pad(r.version, wVersion) +
               Pad(r.available, wAvailable) + (withSource ? r.source : "");
        out += "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
void TestCapturedRealOutput() {
    std::printf("captured real winget output\n");
    const char* text =
        "Name         Id                      Version Available Source\n"
        "-------------------------------------------------------------\n"
        "RipGrep MSVC BurntSushi.ripgrep.MSVC 15.0.0  15.2.0    winget\n"
        "1 upgrades available.\n";

    const auto pkgs = ParseUpgradeOutput(text);
    CheckSize(pkgs.size(), 1, "row count");
    if (pkgs.size() == 1) {
        CheckW(pkgs[0].name, L"RipGrep MSVC", "name");
        CheckW(pkgs[0].id, L"BurntSushi.ripgrep.MSVC", "id");
        CheckW(pkgs[0].current, L"15.0.0", "current version");
        CheckW(pkgs[0].available, L"15.2.0", "available version");
        CheckW(pkgs[0].source, L"winget", "source");
    }
}

void TestNoUpdates() {
    std::printf("no updates available\n");
    const auto pkgs = ParseUpgradeOutput(
        "No installed package found matching input criteria.\n");
    CheckSize(pkgs.size(), 0, "empty result");
}

void TestGeneratedTable() {
    std::printf("generated table with several rows\n");
    const auto text = MakeTable({
        {"Alpha", "Vendor.Alpha", "1.0.0", "2.0.0", "winget"},
        {"Beta Tool", "Vendor.Beta", "3.1", "3.2", "winget"},
        {"Gamma", "Other.Gamma", "0.9", "1.0", "msstore"},
    });
    const auto pkgs = ParseUpgradeOutput(text);
    CheckSize(pkgs.size(), 3, "row count");
    if (pkgs.size() == 3) {
        CheckW(pkgs[0].name, L"Alpha", "row 0 name");
        CheckW(pkgs[0].id, L"Vendor.Alpha", "row 0 id");
        CheckW(pkgs[0].current, L"1.0.0", "row 0 current");
        CheckW(pkgs[0].available, L"2.0.0", "row 0 available");
        CheckW(pkgs[1].name, L"Beta Tool", "row 1 name (has a space)");
        CheckW(pkgs[1].available, L"3.2", "row 1 available");
        CheckW(pkgs[2].source, L"msstore", "row 2 source");
    }
}

void TestUtf8Name() {
    std::printf("multi-byte name must not shift columns\n");
    // Build the table with an ASCII name of the same character count, then
    // swap in a UTF-8 name (same characters, one extra byte) - exactly the
    // situation winget creates when padding by display width.
    std::string text = MakeTable({{"Cafe Tool", "Vendor.TestApp", "1.0.0", "2.0.0", "winget"}});
    const size_t at = text.find("Cafe Tool");
    Check(at != std::string::npos, "placeholder present");
    text.replace(at, 9, "Caf\xC3\xA9 Tool");

    const auto pkgs = ParseUpgradeOutput(text);
    CheckSize(pkgs.size(), 1, "row count");
    if (pkgs.size() == 1) {
        CheckW(pkgs[0].name, L"Caf\xE9 Tool", "multi-byte name");
        CheckW(pkgs[0].id, L"Vendor.TestApp", "id after multi-byte name");
        CheckW(pkgs[0].available, L"2.0.0", "available after multi-byte name");
    }
}

void TestTwoSections() {
    std::printf("table repeated after the explicit-targeting notice\n");
    std::string text = MakeTable({{"Alpha", "Vendor.Alpha", "1.0.0", "2.0.0", "winget"}});
    text += "1 upgrades available.\n\n";
    text += "The following packages have an upgrade available, but require "
            "explicit targeting using the --id option.\n";
    text += MakeTable({{"Beta", "Vendor.Beta", "1.0.0", "3.0.0", "winget"}});

    const auto pkgs = ParseUpgradeOutput(text);
    CheckSize(pkgs.size(), 2, "rows from both sections");
    if (pkgs.size() == 2) {
        CheckW(pkgs[0].id, L"Vendor.Alpha", "first section id");
        CheckW(pkgs[1].id, L"Vendor.Beta", "second section id");
        CheckW(pkgs[1].name, L"Beta", "second section name");
    }
}

void TestNoSourceColumn() {
    std::printf("table without a Source column\n");
    const auto text = MakeTable({{"Alpha", "Vendor.Alpha", "1.0.0", "2.0.0", ""}}, false);
    const auto pkgs = ParseUpgradeOutput(text);
    CheckSize(pkgs.size(), 1, "row count");
    if (pkgs.size() == 1) {
        CheckW(pkgs[0].id, L"Vendor.Alpha", "id");
        CheckW(pkgs[0].available, L"2.0.0", "available is last column");
    }
}

void TestBannerBeforeTable() {
    std::printf("banner text before the table\n");
    std::string text =
        "The source requires that you view the following agreements before using.\n"
        "Terms of Transaction: https://example.invalid/terms\n\n";
    text += MakeTable({{"Alpha", "Vendor.Alpha", "1.0.0", "2.0.0", "winget"}});
    const auto pkgs = ParseUpgradeOutput(text);
    CheckSize(pkgs.size(), 1, "row count");
    if (pkgs.size() == 1) CheckW(pkgs[0].id, L"Vendor.Alpha", "id");
}

void TestHeaderWordsInData() {
    std::printf("notice line with header-shaped words is ignored\n");
    std::string text = MakeTable({{"Alpha", "Vendor.Alpha", "1.0.0", "2.0.0", "winget"}});
    // A data-line-positioned banner carrying the heading words: without the
    // dash-rule guard it would reset the column offsets and lose later rows.
    text += "  Name Id Version Available are column headers.  \n";
    text += MakeTable({{"Beta", "Vendor.Beta", "1.0.0", "3.0.0", "winget"}});

    const auto pkgs = ParseUpgradeOutput(text);
    CheckSize(pkgs.size(), 2, "rows from both sections");
    if (pkgs.size() == 2) {
        CheckW(pkgs[0].id, L"Vendor.Alpha", "first section id");
        CheckW(pkgs[1].id, L"Vendor.Beta", "second section id after fake header");
    }
}

void TestDuplicates() {
    std::printf("duplicate ids are collapsed\n");
    std::string text = MakeTable({
        {"Alpha", "Vendor.Alpha", "1.0.0", "2.0.0", "winget"},
        {"Alpha again", "Vendor.Alpha", "1.0.0", "2.0.0", "winget"},
    });
    const auto pkgs = ParseUpgradeOutput(text);
    CheckSize(pkgs.size(), 1, "row count");
}

void TestAnsiStripping() {
    std::printf("ANSI escape removal\n");
    Check(StripAnsi("\x1b[32mOK\x1b[0m") == "OK", "colour codes removed");
    Check(StripAnsi("\x1b[?25lhidden\x1b[?25h") == "hidden", "cursor codes removed");
    Check(StripAnsi("plain text") == "plain text", "plain text untouched");
    Check(StripAnsi("a\tb") == "a\tb", "tabs preserved");
}

}  // namespace

int main() {
    std::printf("winget parser tests\n\n");
    TestCapturedRealOutput();
    TestNoUpdates();
    TestGeneratedTable();
    TestUtf8Name();
    TestTwoSections();
    TestNoSourceColumn();
    TestBannerBeforeTable();
    TestHeaderWordsInData();
    TestDuplicates();
    TestAnsiStripping();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}