// Resource ids shared between payload.rc and payload.cpp.
#pragma once

#define IDI_TJSETUP              1

#define IDR_TJ_LOADER          101
#define IDR_TJ_HYBRID          102
#define IDR_MSVCP140           103
#define IDR_MSVCP140_1         104
#define IDR_MSVCP140_2         105
#define IDR_MSVCP140_ATOMIC    106
#define IDR_VCRUNTIME140       107
#define IDR_VCRUNTIME140_THR   108
#define IDR_CONCRT140          109
#define IDR_README             110
// The Android half: an UNSIGNED, all-STORED template apk — ours, no game data. Absent when
// the installer is built on a machine with no Android SDK (see TJ_HAVE_APK). The SIGNING KEY
// is deliberately not here: it is generated per player at first use (apk_build.cpp).
#define IDR_APK_TEMPLATE       111
// The Arabic pack (glyph art + translated strings). OPTIONAL, exactly like the Android
// template: produced by port/tools/arabic_font.py, and when it is absent at configure time
// the installer simply ships no Arabic and the game stays English.
#define IDR_ARABIC_FONT        112

#define IDD_MAIN               200
#define IDC_ISO_PATH           201
#define IDC_ISO_BROWSE         202
#define IDC_DEST_PATH          203
#define IDC_DEST_BROWSE        204
#define IDC_SC_DESKTOP         205
#define IDC_SC_STARTMENU       206
#define IDC_STATUS             207
#define IDC_PROGRESS           208
#define IDC_INSTALL            209
#define IDC_INTRO              210
#define IDC_ISO_LABEL          211
#define IDC_DEST_LABEL         212
#define IDC_ANDROID            213

#define IDD_UNINSTALL          220
#define IDC_UN_TEXT            221
#define IDC_UN_KEEPSAVES       222

// NOTE: d3dcompiler_47.dll is deliberately NOT here. make_dist.ps1 copies it out of the build
// machine's SysWOW64, but it is NOT a redistributable -- shipping it in a public download is a
// licensing problem, and it is inbox on Windows 10 and 11 anyway.
