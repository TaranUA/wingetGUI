#pragma once

namespace settings {

enum ThemeMode {
    ThemeSystem = 0,  // follow the Windows light/dark setting
    ThemeLight  = 1,
    ThemeDark   = 2,
};

struct Values {
    int theme = ThemeSystem;
    bool runAsAdmin = false;
};

// An existing LocalAppData copy takes precedence. Otherwise use the portable
// INI and fall back to LocalAppData if the portable file cannot be replaced.
Values Load();
bool Save(const Values& values);

}  // namespace settings
