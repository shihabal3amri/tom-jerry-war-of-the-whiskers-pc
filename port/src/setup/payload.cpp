// Unpack the embedded runtime. See payload.h.
#include "setup/payload.h"
#include "setup/payload_ids.h"

#include <windows.h>
#include <cstdio>
#include <vector>

namespace tj::setup {

namespace {

struct Item { int id; const wchar_t* name; bool optional = false; };

// The VC++ runtime is NOT part of Windows, so app-local copies are the supported way to avoid
// making the player install a redist. The UCRT, d3d11, dxgi, ws2_32, xinput1_4 and xaudio2_9
// are all inbox on Windows 10/11 and are deliberately absent -- as is d3dcompiler_47.dll,
// which is inbox too and is not redistributable.
const Item kItems[] = {
    { IDR_TJ_LOADER,        L"Tom and Jerry - War of the Whiskers.exe" },
    { IDR_TJ_HYBRID,        L"tj_hybrid.dll" },
    { IDR_MSVCP140,         L"msvcp140.dll" },
    { IDR_MSVCP140_1,       L"msvcp140_1.dll" },
    { IDR_MSVCP140_2,       L"msvcp140_2.dll" },
    { IDR_MSVCP140_ATOMIC,  L"msvcp140_atomic_wait.dll" },
    { IDR_VCRUNTIME140,     L"vcruntime140.dll" },
    { IDR_VCRUNTIME140_THR, L"vcruntime140_threads.dll" },
    { IDR_CONCRT140,        L"concrt140.dll" },
    { IDR_README,           L"README.txt" },
    // Optional: absent when the installer was built without an Arabic pack.
    { IDR_ARABIC_FONT,      L"arabic_font.bin", true },
};

bool FindRes(int id, const void*& data, DWORD& size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    data = LockResource(g);
    size = SizeofResource(nullptr, r);
    return data && size;
}

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size, std::string& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "Could not write to the install folder. Try running the installer as "
              "administrator, or choose a different location.";
        return false;
    }
    const uint8_t* p = (const uint8_t*)data;
    size_t done = 0;
    while (done < size) {
        DWORD put = 0;
        DWORD want = (DWORD)((size - done) > (1u << 20) ? (1u << 20) : (size - done));
        if (!WriteFile(h, p + done, want, &put, nullptr) || put != want) {
            CloseHandle(h);
            err = "Could not write to the install folder (the disk may be full).";
            return false;
        }
        done += put;
    }
    CloseHandle(h);
    return true;
}

} // namespace

const wchar_t kGameExeName[] = L"Tom and Jerry - War of the Whiskers.exe";

uint64_t PayloadBytes() {
    uint64_t t = 0;
    for (const Item& it : kItems) {
        const void* d = nullptr; DWORD n = 0;
        if (FindRes(it.id, d, n)) t += n;
    }
    return t;
}

bool WritePayload(const wchar_t* destDir, int width, int height, int mode, std::string& err) {
    for (const Item& it : kItems) {
        const void* d = nullptr; DWORD n = 0;
        if (!FindRes(it.id, d, n)) {
            if (it.optional) continue;
            err = "The installer is damaged (a required file is missing from it). "
                  "Please download it again.";
            return false;
        }
        if (!WriteFileBytes(std::wstring(destDir) + L"\\" + it.name, d, n, err)) return false;
    }

    // NO PLAY.cmd. The dist folder ships one because its exe is the bare "tj_loader.exe", but
    // in an install it is pure confusion -- a folder containing both a .cmd and an .exe, with
    // the .exe being the one that looks unofficial. The named exe above IS the launcher.

    // Shipped display defaults. No [Player] Name: each PC picks up its own computer name the
    // first time, so two LAN lobby rows are not both called the same thing.
    char ini[128];
    int n = _snprintf_s(ini, sizeof ini, _TRUNCATE,
                        "[Display]\r\nWidth=%d\r\nHeight=%d\r\nMode=%d\r\n", width, height, mode);
    if (!WriteFileBytes(std::wstring(destDir) + L"\\tomjerry.ini", ini, (size_t)n, err))
        return false;

    return true;
}

} // namespace tj::setup
