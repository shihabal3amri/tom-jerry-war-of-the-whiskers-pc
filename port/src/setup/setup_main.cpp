// tj_setup.exe -- the installer for the native PC port.
//
// IT SHIPS NO GAME DATA. The player supplies their own disc image and everything that came off
// the disc is extracted from it at install time; the only files carried inside this executable
// are ours (the loader, the hybrid DLL, the VC++ runtime, a readme and an original icon). That
// is what makes publishing it legal.
//
// Install layout:
//     <Program Files>\Tom and Jerry War of the Whiskers\   game data + runtime (read-only)
//     %LOCALAPPDATA%\TomJerryWOW\                          saves + tomjerry.ini (writable)
// The split matters: Program Files is read-only for a standard user, so a save written beside
// the exe fails silently. file_io.cpp's UserDataDir()/SaveRoot() own that split at runtime.
//
// Uninstalling deletes the install folder but LEAVES THE SAVES ALONE unless the player ticks
// the box. Saved games have been destroyed by tooling on this project three times already;
// the uninstaller is not going to be the fourth.
#include "setup/payload_ids.h"
#include "setup/payload.h"
#include "setup/apk_build.h"
#include "setup/xdvdfs.h"
#include "setup/xmf_inject.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <objbase.h>
#include <cwctype>

#include <string>
#include <vector>

using namespace tj::setup;

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)

static const wchar_t kAppName[]     = L"Tom & Jerry: War of the Whiskers";
static const wchar_t kFolderName[]  = L"Tom and Jerry War of the Whiskers";
static const wchar_t kRegKey[]      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TomJerryWOW";
static const wchar_t kShortcut[]    = L"Tom & Jerry - War of the Whiskers.lnk";
static const wchar_t kUninstallExe[] = L"uninstall.exe";
static const wchar_t kUserDataDir[] = L"TomJerryWOW";      // under %LOCALAPPDATA%

// ---------------------------------------------------------------- shared state

struct InstallJob {
    HWND         dlg = nullptr;
    std::wstring iso, dest;
    bool         scDesktop = true, scStartMenu = true;
    bool         android = false;      // also write the side-load folder
    volatile LONG cancel = 0;

    CRITICAL_SECTION lock;
    int          pct = 0;
    std::wstring status;
    bool         ok = false;
    std::wstring error;
    HANDLE       log = INVALID_HANDLE_VALUE;   // /silent only

    void Set(int p, const std::wstring& s) {
        EnterCriticalSection(&lock);
        pct = p; status = s;
        LeaveCriticalSection(&lock);
        if (dlg) {
            PostMessageW(dlg, WM_APP_PROGRESS, 0, 0);
        } else if (log != INVALID_HANDLE_VALUE) {
            // Convert explicitly rather than with "%S": the status strings carry an ellipsis
            // and an em dash, which %S cannot represent in the narrow locale -- it fails, the
            // length comes back as -1, and every interesting line is silently dropped.
            std::wstring w = s + L"\r\n";
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string utf8((size_t)(n > 0 ? n : 0), '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &utf8[0], n, nullptr, nullptr);
            char head[16];
            int hn = _snprintf_s(head, sizeof head, _TRUNCATE, "[%3d%%] ", p);
            DWORD put = 0;
            if (hn > 0) WriteFile(log, head, (DWORD)hn, &put, nullptr);
            if (!utf8.empty()) WriteFile(log, utf8.data(), (DWORD)utf8.size(), &put, nullptr);
        }
    }
};

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------------------------------------------------------- filesystem helpers

static bool MakeDirs(const std::wstring& dir) {
    for (size_t i = 3; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') {
            std::wstring part = dir.substr(0, i);
            if (!CreateDirectoryW(part.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                if (GetFileAttributesW(part.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
            }
        }
    }
    return true;
}

// Recursive delete. `keepSaves` protects a _SAVES folder anywhere in the tree -- an install
// made by an older build could still have one beside the exe, and losing it would be exactly
// the failure this project has already had three times.
static void UnLog(const wchar_t* fmt, ...);

// Is this folder actually one of our installs? The uninstaller deletes a whole directory
// tree, so it must never take a path on trust -- not from its own location, and especially
// not from a registry value that could be stale or point somewhere else entirely.
//
// The markers are `tj_hybrid.dll` and `default.xbe`: names that do NOT change. An earlier
// version probed for "tj_loader.exe", which stopped existing the moment the installed game
// exe was given a friendly name -- so a correctly-placed uninstaller decided it was not in an
// install, fell through to the registry, and aimed at a different directory entirely.
static bool LooksLikeInstall(const std::wstring& dir) {
    if (dir.size() < 4) return false;
    return GetFileAttributesW((dir + L"\\tj_hybrid.dll").c_str()) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW((dir + L"\\default.xbe").c_str())  != INVALID_FILE_ATTRIBUTES;
}

static void DeleteTree(const std::wstring& dir, bool keepSaves) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        UnLog(L"    FindFirstFile(\"%s\\*\") failed, err %lu", dir.c_str(), GetLastError());
        return;
    }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (keepSaves && _wcsicmp(fd.cFileName, L"_SAVES") == 0) continue;
            DeleteTree(p, keepSaves);
        } else {
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(p.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir.c_str());        // fails harmlessly if _SAVES was kept
}

static uint64_t DirSizeKb(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += DirSizeKb(p) * 1024ull;
        else total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total / 1024ull;
}

static std::wstring KnownDir(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) { out = p; CoTaskMemFree(p); }
    return out;
}

// ---------------------------------------------------------------- shortcuts

static bool MakeShortcut(const std::wstring& lnkPath, const std::wstring& target,
                         const std::wstring& workDir, const std::wstring& iconPath) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link))) return false;
    bool ok = false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetDescription(L"Tom & Jerry: War of the Whiskers");
    if (!iconPath.empty()) link->SetIconLocation(iconPath.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(lnkPath.c_str(), TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

// ---------------------------------------------------------------- the install worker

static bool ProgressCb(void* ctx, const char* file, uint64_t done, uint64_t total) {
    InstallJob* job = (InstallJob*)ctx;
    if (InterlockedCompareExchange(&job->cancel, 0, 0)) return false;
    // Extraction owns 5..85% of the bar; it is the overwhelming majority of the wall clock.
    int pct = 5 + (int)(total ? (done * 80 / total) : 80);
    static uint64_t lastShown = 0;
    if (done - lastShown > (2u << 20) || done == total) {
        lastShown = done;
        job->Set(pct, L"Copying game files from the disc image\x2026  " + Widen(file ? file : ""));
    }
    return true;
}

// Fails the job with a user-facing reason and returns false, so every failure path is one line.
static bool Fail(InstallJob* job, const std::wstring& why) {
    job->error = why;
    job->Set(job->pct, L"Failed: " + why);
    return false;
}

static bool WriteTextFileUtf8(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr);
    CloseHandle(h);
    return ok && put == text.size();
}

// ---------------------------------------------------------------- the Android build
//
// Produces ONE self-contained apk: the player side-loads it and plays. No Android SDK on this
// machine, no adb, no copying game folders by hand.
//
// It works because the installer carries an UNSIGNED template apk (manifest, resources, the
// two native libraries — ours, no game data, so it is publishable on the same footing as
// tj_hybrid.dll) and does the last two steps itself: add the player's just-extracted game
// files as one uncompressed entry, then write a v1 (JAR) signature over the result. See
// apk_build.cpp for why v1 is sufficient and why the manifest targets API 29.
//
// The game files come from the PC install that was just made, AFTER the MEAT RUSH injection,
// so the phone and the PC end up with byte-identical data — which is what lets them see each
// other's LAN games instead of refusing with DIFFERENT VERSION.
#if TJ_HAVE_APK
static bool LoadRes(int id, const void*& data, size_t& bytes) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    data = LockResource(g);
    bytes = SizeofResource(nullptr, r);
    return data && bytes;
}

static bool ApkProgress(void* ctx, const wchar_t* stage, uint64_t done, uint64_t total) {
    InstallJob* job = (InstallJob*)ctx;
    if (!job) return true;
    if (InterlockedCompareExchange(&job->cancel, 0, 0)) return false;
    // 90..96% of the bar: the PC install owns everything before it.
    int pct = 90 + (total ? (int)(done * 6 / total) : 6);
    job->Set(pct, stage);
    return true;
}

static bool BuildAndroidVersion(InstallJob* job, const std::wstring& dest, std::wstring& why) {
    const void* tpl = nullptr; size_t tplN = 0;
    if (!LoadRes(IDR_APK_TEMPLATE, tpl, tplN)) {
        why = L"this installer was built without the Android template";
        return false;
    }
    // The signing key lives with the PLAYER, not with the installer, so a later installer
    // build produces an apk that UPDATES the one already on their phone. First run creates it.
    std::wstring keyDir = KnownDir(FOLDERID_LocalAppData);
    if (keyDir.empty()) { why = L"the local app data folder could not be found"; return false; }
    keyDir += L"\\";
    keyDir += kUserDataDir;
    MakeDirs(keyDir);
    const std::wstring keyFile = keyDir + L"\\android-signing.pfx";
    const std::wstring dir = dest + L"\\Android";
    if (!MakeDirs(dir)) { why = L"the Android folder could not be created"; return false; }

    // Exactly what the phone needs, and nothing else: the four items the LAN data hash covers,
    // plus the Arabic pack. tomjerry.ini, the runtime DLLs and _SAVES are PC-side and
    // deliberately excluded.
    //
    // The pack sits at the ROOT of the packed tree because that is where arabic.cpp looks for
    // it on Android: the executable there is libtjgame.so in the app's read-only native-lib
    // dir, so the beside-the-exe lookup that serves the PC install cannot work and the asset
    // root is used instead. It does NOT affect the LAN data hash, which covers default.xbe and
    // the three asset trees only -- so a phone and a PC still match. An installer built with no
    // pack simply skips it: the walk ignores names that do not exist.
    static const wchar_t* kItems[] = { L"default.xbe", L"GFX", L"AUDMUSIC", L"AUDSoundFX",
                                       L"arabic_font.bin" };
    const std::wstring apk = dir + L"\\Tom and Jerry - War of the Whiskers.apk";
    std::string err;
    if (!tj::setup::BuildAndroidApk(tpl, tplN, keyFile, dest,
                                    kItems, (int)(sizeof kItems / sizeof kItems[0]),
                                    apk, &ApkProgress, job, err)) {
        why = Widen(err);
        DeleteFileW(apk.c_str());          // never leave a half-written apk to be side-loaded
        return false;
    }

    std::string r;
    r += "TOM & JERRY: WAR OF THE WHISKERS - ANDROID\r\n";
    r += "==========================================\r\n\r\n";
    r += "\"Tom and Jerry - War of the Whiskers.apk\" in this folder is the complete game,\r\n";
    r += "built from YOUR disc image. Nothing else needs to be copied.\r\n\r\n";
    r += "TO PLAY\r\n";
    r += "  1. Copy the .apk to your phone and open it. Android will ask you to allow\r\n";
    r += "     installing from this source; that is the normal side-load prompt.\r\n";
    r += "  2. Open the app. The FIRST launch unpacks the game files and takes a short\r\n";
    r += "     while on a black screen - this happens once.\r\n";
    r += "  3. Play. Touch controls work; a Bluetooth or USB gamepad also works.\r\n\r\n";
    r += "WHAT YOUR PHONE NEEDS\r\n";
    r += "  * 64-bit ARM (arm64-v8a). Any modern chip works - Snapdragon, Exynos, Tensor,\r\n";
    r += "    Dimensity, Kirin. 32-bit-only phones and x86 emulators are NOT supported.\r\n";
    r += "  * Android 8.0 or newer, and OpenGL ES 3.0 (universal on 64-bit Android).\r\n";
    r += "  * About 500 MB free: the app, plus the game files it unpacks on first launch.\r\n\r\n";
    r += "LAN PLAY WITH THE PC\r\n";
    r += "  The phone and this PC can play each other over the same Wi-Fi, because both were\r\n";
    r += "  made by this installer from the same disc image. If a listed game ever shows\r\n";
    r += "  DIFFERENT VERSION, the two ends were installed from different builds.\r\n\r\n";
    r += "SAVES\r\n";
    r += "  The phone keeps its saves in the app's own storage, separate from the PC's, so\r\n";
    r += "  uninstalling the app removes them.\r\n";
    WriteTextFileUtf8(dir + L"\\README.txt", r);
    return true;
}
#endif  // TJ_HAVE_APK

static bool RunInstall(InstallJob* job) {
    std::string err;

    job->Set(1, L"Reading the disc image\x2026");
    XdvdfsImage img;
    if (!img.Open(job->iso.c_str(), err)) return Fail(job, Widen(err));

    // Identity + completeness. The volume timestamp is the reliable "is this the right game"
    // test: it is identical across re-masterings, whereas default.xbe legitimately differs
    // between images, so a hash pinned to one value would reject real discs.
    if (img.VolumeFileTime() != kTomJerryVolumeTime)
        return Fail(job, L"That disc image is not Tom & Jerry: War of the Whiskers.");

    std::vector<const IsoEntry*> wanted;
    uint64_t wantedBytes = 0;
    for (const IsoEntry& e : img.Entries()) {
        if (e.dir) continue;
        bool take = _stricmp(e.path.c_str(), kWantedFile) == 0;
        for (const char* top : kWantedTopLevel) {
            size_t n = strlen(top);
            if (_strnicmp(e.path.c_str(), top, (int)n) == 0 && e.path[n] == '\\') take = true;
        }
        if (take) { wanted.push_back(&e); wantedBytes += e.size; }
    }
    if (img.FileCount() != kExpectedFiles || img.TotalFileBytes() != kExpectedBytes) {
        // Not fatal: a re-mastered image can differ. Say so rather than refuse outright.
        job->Set(2, L"Note: this image does not exactly match a retail disc; continuing\x2026");
        Sleep(900);
    }

    // Enough room? 220 MB of assets plus the runtime, with headroom.
    ULARGE_INTEGER freeAvail = {};
    std::wstring driveRoot = job->dest.substr(0, 3);
    if (GetDiskFreeSpaceExW(driveRoot.c_str(), &freeAvail, nullptr, nullptr)) {
        uint64_t need = wantedBytes + PayloadBytes() + (64ull << 20);
        if (freeAvail.QuadPart < need) {
            wchar_t msg[256];
            swprintf_s(msg, L"There is not enough free space on that drive. About %llu MB is needed.",
                       (unsigned long long)(need >> 20));
            return Fail(job, msg);
        }
    }

    job->Set(3, L"Creating the install folder\x2026");
    if (!MakeDirs(job->dest))
        return Fail(job, L"Could not create the install folder. Try a different location.");

    if (!img.Extract(wanted, job->dest.c_str(), ProgressCb, job, err))
        return Fail(job, Widen(err));

    job->Set(86, L"Installing the game runtime\x2026");
    if (!WritePayload(job->dest.c_str(), 1280, 720, 0, err))
        return Fail(job, Widen(err));

    // Installing over an earlier version leaves its files behind, and the two that matter are
    // the ones this build deliberately stopped shipping: the old unnamed launcher and the .cmd
    // shim. Leaving them means the player still sees "tj_loader.exe" and "PLAY.cmd" next to the
    // new named exe and has no idea which one to run -- which is the exact confusion the rename
    // was meant to end. Named explicitly: never pattern-delete inside a user's folder.
    // tj_log.txt joins the list: the log moved to the user data folder (Program Files is not
    // writable by a standard user, and trying to open it there killed the game on launch), so
    // one left here is stale and only misleads a crash report.
    for (const wchar_t* stale : { L"tj_loader.exe", L"PLAY.cmd", L"tj_log.txt" }) {
        std::wstring p = job->dest + L"\\" + stale;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(p.c_str());
        }
    }

    // MEAT RUSH needs the same turkey leg in every arena, and `WPH2` is a per-arena NAME SLOT
    // (a bone in Hell, a rapier in Cabin, absent in seven arenas). KITCHEN's mesh is copied
    // into every arena's object file as "MEAT". Without this the meat has no model in 12 of
    // the 13 arenas. Byte-identical to what make_dist.ps1 produces, which is what lets an
    // installed copy and a dev copy pass the LAN data-hash check against each other.
    job->Set(90, L"Preparing MEAT RUSH assets\x2026");
    MeatInjectResult mr = InjectMeatIntoAllArenas(job->dest.c_str());
    if (mr.failed)
        return Fail(job, L"The game files could not be prepared: " + Widen(mr.firstError));

#if TJ_HAVE_APK
    if (job->android) {
        job->Set(90, L"Building the Android app\x2026");
        std::wstring why;
        if (!BuildAndroidVersion(job, job->dest, why))
            return Fail(job, L"The Android version could not be built: " + why +
                             L". The PC game is installed and working.");
    }
#endif

    job->Set(97, L"Creating shortcuts\x2026");
    // The installer copies itself in as the uninstaller, so Add/Remove Programs has a stable
    // target that does not depend on wherever the download happened to be run from.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring uninst = job->dest + L"\\" + kUninstallExe;
    CopyFileW(self, uninst.c_str(), FALSE);

    // Shortcuts point at the game exe itself, and take their icon from it -- it carries the
    // icon as a resource, so the shortcut, the taskbar and the folder listing all agree.
    std::wstring game = job->dest + L"\\" + kGameExeName;
    if (job->scDesktop) {
        std::wstring d = KnownDir(FOLDERID_Desktop);
        if (!d.empty()) MakeShortcut(d + L"\\" + kShortcut, game, job->dest, game);
    }
    if (job->scStartMenu) {
        std::wstring s = KnownDir(FOLDERID_CommonPrograms);
        if (s.empty()) s = KnownDir(FOLDERID_Programs);
        if (!s.empty()) MakeShortcut(s + L"\\" + kShortcut, game, job->dest, game);
    }

    job->Set(99, L"Registering with Add or remove programs\x2026");
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)
            == ERROR_SUCCESS) {
        auto S = [&](const wchar_t* n, const std::wstring& v) {
            RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(),
                           (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        };
        auto D = [&](const wchar_t* n, DWORD v) {
            RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, sizeof v);
        };
        S(L"DisplayName", kAppName);
        S(L"DisplayVersion", L"1.0.0");
        S(L"Publisher", L"Native PC port");
        S(L"InstallLocation", job->dest);
        S(L"DisplayIcon", uninst);
        S(L"UninstallString", L"\"" + uninst + L"\" /uninstall");
        S(L"QuietUninstallString", L"\"" + uninst + L"\" /uninstall /quiet");
        D(L"NoModify", 1);
        D(L"NoRepair", 1);
        D(L"EstimatedSize", (DWORD)DirSizeKb(job->dest));
        RegCloseKey(k);
    }

    job->ok = true;
    job->Set(100, L"Done.");
    return true;
}

static DWORD WINAPI InstallThread(LPVOID param) {
    InstallJob* job = (InstallJob*)param;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    RunInstall(job);
    PostMessageW(job->dlg, WM_APP_DONE, 0, 0);
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------- pickers

static std::wstring PickFile(HWND owner) {
    IFileOpenDialog* dlg = nullptr;
    std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog, (void**)&dlg))) return out;
    COMDLG_FILTERSPEC f[] = { { L"Xbox disc image (*.iso)", L"*.iso" }, { L"All files", L"*.*" } };
    dlg->SetFileTypes(2, f);
    dlg->SetTitle(L"Choose your Tom & Jerry: War of the Whiskers disc image");
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

static std::wstring PickFolder(HWND owner, const std::wstring& initial) {
    IFileOpenDialog* dlg = nullptr;
    std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog, (void**)&dlg))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose where to install the game");
    (void)initial;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

// ---------------------------------------------------------------- install dialog

static InstallJob g_job;
static HANDLE     g_thread = nullptr;
static bool       g_running = false;

static void SetControlsEnabled(HWND dlg, bool on) {
    for (int id : { IDC_ISO_PATH, IDC_ISO_BROWSE, IDC_DEST_PATH, IDC_DEST_BROWSE,
                    IDC_SC_DESKTOP, IDC_SC_STARTMENU, IDC_ANDROID, IDC_INSTALL })
        EnableWindow(GetDlgItem(dlg, id), on);
}

static INT_PTR CALLBACK MainProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SendMessageW(dlg, WM_SETICON, ICON_BIG,
                     (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_TJSETUP)));
        SendMessageW(dlg, WM_SETICON, ICON_SMALL,
                     (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_TJSETUP)));
        CheckDlgButton(dlg, IDC_SC_DESKTOP, BST_CHECKED);
        CheckDlgButton(dlg, IDC_SC_STARTMENU, BST_CHECKED);
        std::wstring pf = KnownDir(FOLDERID_ProgramFilesX86);
        if (pf.empty()) pf = KnownDir(FOLDERID_ProgramFiles);
        SetDlgItemTextW(dlg, IDC_DEST_PATH, (pf + L"\\" + kFolderName).c_str());
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETRANGE32, 0, 100);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ISO_BROWSE: {
            std::wstring p = PickFile(dlg);
            if (!p.empty()) SetDlgItemTextW(dlg, IDC_ISO_PATH, p.c_str());
            return TRUE;
        }
        case IDC_DEST_BROWSE: {
            wchar_t cur[MAX_PATH] = {};
            GetDlgItemTextW(dlg, IDC_DEST_PATH, cur, MAX_PATH);
            std::wstring p = PickFolder(dlg, cur);
            if (!p.empty()) SetDlgItemTextW(dlg, IDC_DEST_PATH, (p + L"\\" + kFolderName).c_str());
            return TRUE;
        }
        case IDC_INSTALL: {
            wchar_t iso[MAX_PATH] = {}, dest[MAX_PATH] = {};
            GetDlgItemTextW(dlg, IDC_ISO_PATH, iso, MAX_PATH);
            GetDlgItemTextW(dlg, IDC_DEST_PATH, dest, MAX_PATH);
            if (!iso[0] || GetFileAttributesW(iso) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(dlg, L"Choose your disc image first.", kAppName, MB_ICONINFORMATION);
                return TRUE;
            }
            if (!dest[0] || wcslen(dest) < 3 || dest[1] != L':') {
                MessageBoxW(dlg, L"Choose a full path to install to, for example "
                                 L"C:\\Program Files (x86)\\Tom and Jerry War of the Whiskers.",
                            kAppName, MB_ICONINFORMATION);
                return TRUE;
            }
            InitializeCriticalSection(&g_job.lock);
            g_job.dlg = dlg;
            g_job.iso = iso;
            g_job.dest = dest;
            g_job.scDesktop = IsDlgButtonChecked(dlg, IDC_SC_DESKTOP) == BST_CHECKED;
            g_job.scStartMenu = IsDlgButtonChecked(dlg, IDC_SC_STARTMENU) == BST_CHECKED;
            g_job.android = IsDlgButtonChecked(dlg, IDC_ANDROID) == BST_CHECKED;
            g_job.cancel = 0;
            SetControlsEnabled(dlg, false);
            g_running = true;
            g_thread = CreateThread(nullptr, 0, InstallThread, &g_job, 0, nullptr);
            return TRUE;
        }
        case IDCANCEL:
            if (g_running) {
                if (MessageBoxW(dlg, L"Stop the installation?", kAppName,
                                MB_YESNO | MB_ICONQUESTION) == IDYES)
                    InterlockedExchange(&g_job.cancel, 1);
                return TRUE;
            }
            EndDialog(dlg, 0);
            return TRUE;
        }
        return FALSE;

    case WM_APP_PROGRESS: {
        EnterCriticalSection(&g_job.lock);
        int pct = g_job.pct;
        std::wstring s = g_job.status;
        LeaveCriticalSection(&g_job.lock);
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, pct, 0);
        SetDlgItemTextW(dlg, IDC_STATUS, s.c_str());
        return TRUE;
    }

    case WM_APP_DONE: {
        g_running = false;
        if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = nullptr; }
        if (g_job.ok) {
            SetDlgItemTextW(dlg, IDC_STATUS, L"Installed.");
            std::wstring msg = std::wstring(kAppName) +
                L" is installed.\n\nSaved games and settings are kept in your user profile, so "
                L"they survive an uninstall.\n\nPlay now?";
            if (MessageBoxW(dlg, msg.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                std::wstring game = g_job.dest + L"\\" + kGameExeName;
                ShellExecuteW(nullptr, L"open", game.c_str(), nullptr, g_job.dest.c_str(), SW_SHOWNORMAL);
            }
            EndDialog(dlg, 1);
        } else {
            SetDlgItemTextW(dlg, IDC_STATUS, L"");
            SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
            MessageBoxW(dlg, g_job.error.empty() ? L"The installation did not finish."
                                                 : g_job.error.c_str(),
                        kAppName, MB_ICONERROR);
            SetControlsEnabled(dlg, true);
        }
        return TRUE;
    }

    case WM_CLOSE:
        if (!g_running) EndDialog(dlg, 0);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- uninstall

struct UninstallState { std::wstring dir; bool alsoSaves = false; };
static UninstallState g_un;

// Uninstalling deletes the folder it is running from, so it cannot log next to itself.
// %TEMP% survives, and "it said it uninstalled but the folder is still there" is exactly the
// report that is impossible to act on without this.
static void UnLog(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path)) return;
    wcscat_s(path, L"tj_uninstall.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t line[1024];
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf_s(line, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) {
        std::wstring w = std::wstring(line) + L"\r\n";
        int b = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string utf8((size_t)(b > 0 ? b : 0), '\0');
        if (b > 0) {
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &utf8[0], b, nullptr, nullptr);
            DWORD put = 0;
            WriteFile(h, utf8.data(), (DWORD)utf8.size(), &put, nullptr);
        }
    }
    CloseHandle(h);
}

static void DoUninstall(HWND dlg) {
    UnLog(L"--- uninstall: dir=\"%s\" alsoSaves=%d", g_un.dir.c_str(), g_un.alsoSaves ? 1 : 0);
    // Shortcuts first: a stale .lnk pointing at a deleted folder is the most visible leftover.
    for (REFKNOWNFOLDERID id : { FOLDERID_Desktop, FOLDERID_CommonPrograms, FOLDERID_Programs }) {
        std::wstring d = KnownDir(id);
        if (!d.empty()) DeleteFileW((d + L"\\" + kShortcut).c_str());
    }
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 40, 0);

    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kRegKey);
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 55, 0);

    // The saves live in the user profile and are NOT ours to delete by default.
    if (g_un.alsoSaves) {
        std::wstring local = KnownDir(FOLDERID_LocalAppData);
        if (!local.empty()) DeleteTree(local + L"\\" + kUserDataDir, false);
    }
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 70, 0);

    // Belt and braces: re-check immediately before the destructive call. Whatever else is
    // wrong, this function must never delete a tree that is not one of our installs.
    if (!g_un.dir.empty() && LooksLikeInstall(g_un.dir)) {
        DeleteTree(g_un.dir, !g_un.alsoSaves);
        UnLog(L"    tree deleted; dir still present = %d",
              GetFileAttributesW(g_un.dir.c_str()) != INVALID_FILE_ATTRIBUTES ? 1 : 0);
    } else {
        UnLog(L"    REFUSED to delete \"%s\" -- not an install folder", g_un.dir.c_str());
    }
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 100, 0);

    // We are running from inside the folder we just deleted, so the exe itself is still
    // locked. Hand the last step to a detached cmd that waits for us to exit.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"/c ping 127.0.0.1 -n 3 >nul & del /f /q \"" + std::wstring(self) +
                       L"\" & rmdir \"" + g_un.dir + L"\"";
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring shell = std::wstring(sys) + L"\\cmd.exe";
    std::wstring line = L"\"" + shell + L"\" " + cmd;
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(0);
    if (CreateProcessW(shell.c_str(), buf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}

static INT_PTR CALLBACK UninstallProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SendMessageW(dlg, WM_SETICON, ICON_BIG,
                     (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_TJSETUP)));
        std::wstring t = std::wstring(kAppName) + L" will be removed from:\n" + g_un.dir +
                         L"\n\nYour saved games are kept unless you tick the box below.";
        SetDlgItemTextW(dlg, IDC_UN_TEXT, t.c_str());
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETRANGE32, 0, 100);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            g_un.alsoSaves = IsDlgButtonChecked(dlg, IDC_UN_KEEPSAVES) == BST_CHECKED;
            if (g_un.alsoSaves &&
                MessageBoxW(dlg, L"Your saved games will be permanently deleted. Continue?",
                            kAppName, MB_YESNO | MB_ICONWARNING) != IDYES)
                return TRUE;
            EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
            EnableWindow(GetDlgItem(dlg, IDCANCEL), FALSE);
            DoUninstall(dlg);
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        return FALSE;
    case WM_CLOSE:
        EndDialog(dlg, 0);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- entry

static bool HasArg(const wchar_t* cmdline, const wchar_t* flag) {
    std::wstring c(cmdline);
    for (auto& ch : c) ch = (wchar_t)towlower(ch);
    return c.find(flag) != std::wstring::npos;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int) {
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();

    // WHICH PROGRAM ARE WE? The installer copies itself in as uninstall.exe, so the SAME binary
    // has to do both jobs. Deciding on "/uninstall" alone was wrong: Explorer passes no
    // arguments, so double-clicking uninstall.exe opened the installer instead of uninstalling.
    // The file name is the reliable signal -- Add/Remove Programs still passes the switch, and
    // now a double-click works too.
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    const wchar_t* selfName = wcsrchr(selfPath, L'\\');
    selfName = selfName ? selfName + 1 : selfPath;
    bool amUninstaller = _wcsicmp(selfName, kUninstallExe) == 0 || HasArg(cmdline, L"/uninstall");

    int rc = 0;
    // /silent <iso> <destination> [/android]  -- the same install with no UI, and a log next
    // to the exe. This is what makes the installer testable without a human clicking through
    // UAC, and it doubles as an unattended install for anyone who wants one. /android adds the
    // side-load folder, so the Android path is testable the same way the rest of it is.
    if (HasArg(cmdline, L"/silent")) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring iso, dest;
        for (int i = 1; i + 1 < argc; ++i) {
            if (_wcsicmp(argv[i], L"/silent") == 0) {
                if (i + 2 < argc) { iso = argv[i + 1]; dest = argv[i + 2]; }
                break;
            }
        }
        if (argv) LocalFree(argv);
        if (iso.empty() || dest.empty()) {
            MessageBoxW(nullptr, L"usage: tj_setup.exe /silent <disc.iso> <install folder>",
                        kAppName, MB_ICONINFORMATION);
            CoUninitialize();
            return 2;
        }
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring logPath = std::wstring(self) + L".log";

        InitializeCriticalSection(&g_job.lock);
        g_job.dlg = nullptr;
        g_job.iso = iso;
        g_job.dest = dest;
        g_job.scDesktop = g_job.scStartMenu = false;   // unattended: no shortcuts by default
        g_job.android = HasArg(cmdline, L"/android");
        g_job.log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_job.log != INVALID_HANDLE_VALUE) {
            DWORD put = 0;                            // BOM: the log is UTF-8, so say so
            WriteFile(g_job.log, "\xEF\xBB\xBF", 3, &put, nullptr);
        }
        // The /android flag has been silently lost twice -- once to a shell that rewrote it
        // into a path, once to a build that compiled the feature out -- and BOTH times the
        // install reported success and produced no apk, which is indistinguishable from the
        // flag never being passed. Record what actually arrived, in the log itself.
        g_job.Set(0, L"[args] <" + std::wstring(cmdline ? cmdline : L"") +
                     L"> android=" + std::wstring(g_job.android ? L"yes" : L"no"));
        bool ok = RunInstall(&g_job);
        if (g_job.log != INVALID_HANDLE_VALUE) CloseHandle(g_job.log);
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (amUninstaller) {
        // Where we were installed: our own folder is authoritative, the registry is the
        // fallback for an uninstaller invoked from somewhere else.
        std::wstring dir = selfPath;
        size_t at = dir.find_last_of(L'\\');
        if (at != std::wstring::npos) dir.resize(at);
        if (!LooksLikeInstall(dir)) {
            // Only now consult the registry -- and validate what it hands back the same way.
            std::wstring fromReg;
            HKEY k = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
                wchar_t v[MAX_PATH] = {}; DWORD n = sizeof v, type = 0;
                if (RegQueryValueExW(k, L"InstallLocation", nullptr, &type, (BYTE*)v, &n) == ERROR_SUCCESS)
                    fromReg = v;
                RegCloseKey(k);
            }
            dir = LooksLikeInstall(fromReg) ? fromReg : std::wstring();
        }
        if (dir.empty()) {
            UnLog(L"--- refused: \"%s\" is not an install folder and the registry did not "
                  L"name a valid one", selfPath);
            MessageBoxW(nullptr,
                        L"This uninstaller could not find the installed game.\n\n"
                        L"Nothing has been changed. Run the uninstaller from inside the "
                        L"game's own folder, or use Settings > Apps.",
                        kAppName, MB_ICONWARNING);
            CoUninitialize();
            return 1;
        }
        g_un.dir = dir;
        if (HasArg(cmdline, L"/quiet")) {
            DoUninstall(nullptr);
            rc = 0;                                   // 0 = success, the shell convention
        } else {
            rc = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_UNINSTALL), nullptr,
                                 UninstallProc, 0) == 1 ? 0 : 1;
        }
    } else {
        rc = (int)DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, MainProc, 0);
    }

    CoUninitialize();
    return rc;
}
