#pragma once

// Single source of truth for resource / control identifiers.

#define IDR_APP_ICON      1

#define IDD_SETTINGS      101

// Main window controls
#define IDC_STATUS        1001
#define IDC_LIST          1002
#define IDC_LOG           1003
#define IDC_BTN_CHECK     1004
#define IDC_BTN_UPDATE    1005
#define IDC_BTN_ALL       1006
#define IDC_BTN_SEL_ALL   1007
#define IDC_BTN_SEL_NONE  1008
#define IDC_BTN_SETTINGS  1009

// Settings dialog controls
#define IDC_THEME_SYSTEM  1101
#define IDC_THEME_LIGHT   1102
#define IDC_THEME_DARK    1103
#define IDC_RUN_ADMIN     1104
#define IDC_SETTINGS_HINT 1105

// windres does not see the SDK's IDC_STATIC definition
#ifndef IDC_STATIC
#define IDC_STATIC (-1)
#endif