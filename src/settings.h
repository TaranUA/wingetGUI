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

// Settings live in WingetGUI.ini next to the executable (portable). If that
// location is not writable, %LOCALAPPDATA%\WingetGUI\settings.ini is used.
Values Load();
void Save(const Values& values);

}  // namespace settings