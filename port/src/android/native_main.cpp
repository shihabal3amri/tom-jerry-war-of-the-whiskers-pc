// WOTW Android app — the COMPOSITOR + INPUT SHELL (Stage 6 subprocess architecture,
// session 28; src/android/ipc_protocol.h has the WHY: the engine cannot share an address
// space with ART, so the game runs in an ART-free exec'd subprocess).
//
// This process owns exactly three things:
//   1. THE WINDOW + GLES: a render thread replays the subprocess's draw-command ring into
//      the real GLES3 device (gles_gfx) on the ANativeWindow, and presents. The EGL context
//      survives window loss (GlesRelease/AttachWindowSurface) — the game keeps running.
//   2. INPUT: Bluetooth-gamepad and touch events are translated to the Xbox pad shape and
//      published into the shared region (seqlock); the subprocess feeds them to PORT 0.
//   3. THE SUBPROCESS: created from the APK's own native-lib dir (the one app-owned
//      location that is exec-allowed on modern Android), handed the shared memfd + the
//      asset path, reaped and killed with the activity.
//
// TOUCH LAYOUT (landscape, invisible zones for v1 — a drawn overlay comes later):
//   left third          : movement — a virtual stick around the zone centre
//   bottom-right corner : A (jump / confirm)      right edge, mid : B (grab / back)
//   right, above A      : X (kick)                top-right corner: START
#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <android/window.h>        // AWINDOW_FLAG_KEEP_SCREEN_ON / _FULLSCREEN
#include <android/asset_manager.h>
#include <aaudio/AAudio.h>         // speaker output for the game's shm PCM ring (minSdk 26)
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <new>
#include <vector>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "runtime/gfx/d3d8.h"
#include "android/ipc_protocol.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "wotw", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "wotw", __VA_ARGS__)

extern char** environ;

namespace tj::gfx {
void SetAndroidDisplaySize(int w, int h);
void GlesSetGameSize(int w, int h);
bool GlesReleaseWindowSurface();
bool GlesAttachWindowSurface(void* nativeWindow);
void GlesSetDrawDiag(uint32_t mask);
uint64_t GlesDrawDiagDrain(uint64_t out[4]);
// One-upload-per-frame streaming (the Android compositor's private path; d3d8.h keeps the
// shared Device contract). GlesStreamUpload puts a whole frame of geometry in the stream
// buffers; the *At draws name their slice by byte offset instead of handing over a pointer.
bool GlesStreamUpload(const void* vb, unsigned long vbBytes, const void* ib, unsigned long ibBytes);
void GlesDrawPCAt(uint32_t vbOfs, int vertexCount);
void GlesDrawPTCAt(uint32_t vbOfs, int vertexCount, uint32_t ibOfs, int indexCount);
void GlesDrawShinyAt(uint32_t vbOfs, int vertexCount, uint32_t ibOfs, int indexCount,
                     TextureHandle t0, TextureHandle t1, TextureHandle t2, TextureHandle t3);
}

namespace {

// The whole hybrid layer (in the SUBPROCESS) logs via printf; the app does too. Pipe both
// this process's stdout/stderr to logcat — and because the subprocess INHERITS fd 1/2
// across exec, its output lands in the same pump. Tag: wotw-game.
uint64_t NowNs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
// CPU time actually burned BY THIS THREAD. The whole display-leg conclusion rests on wall
// clock ("replay costs 38-42 ms/frame"), and wall clock cannot tell work from waiting: a
// descheduled or driver-blocked thread reads exactly like a busy one. Measuring both is the
// only way to know which of the two the compositor is doing.
uint64_t NowCpuNs() {
    timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

void* LogPump(void* pv) {
    int fd = (int)(intptr_t)pv;
    char buf[1024]; int pos = 0;
    for (;;) {
        ssize_t n = read(fd, buf + pos, sizeof(buf) - 1 - pos);
        if (n <= 0) break;
        pos += (int)n; buf[pos] = 0;
        char* start = buf; char* nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0; __android_log_write(ANDROID_LOG_INFO, "wotw-game", start); start = nl + 1;
        }
        int left = (int)(buf + pos - start);
        memmove(buf, start, left); pos = left;
        if (pos >= (int)sizeof(buf) - 1) {
            buf[pos] = 0; __android_log_write(ANDROID_LOG_INFO, "wotw-game", buf); pos = 0;
        }
    }
    return nullptr;
}
void RedirectStdio() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    int pfd[2];
    // O_CLOEXEC: the dup2'd fds 1/2 stay inheritable, but the pipe's own ends must NOT
    // survive exec — a child holding pfd[0] would keep its own stdout pipe alive after the
    // app dies, so its writes never see EPIPE and printf could block forever on a full pipe.
    if (pipe2(pfd, O_CLOEXEC) != 0) return;
    dup2(pfd[1], STDOUT_FILENO);      // fds 1/2 survive exec -> subprocess logs too
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);
    pthread_t t;
    if (pthread_create(&t, nullptr, &LogPump, (void*)(intptr_t)pfd[0]) == 0) pthread_detach(t);
}

// ---- shared region + subprocess ------------------------------------------------------------
tj::ipc::Header* g_hdr = nullptr;
uint8_t*         g_ring = nullptr;
int              g_shmFd = -1;
pid_t            g_child = 0;
int              g_gameW = 0, g_gameH = 0;    // the game's render res (landscape dims)

bool CreateShm(int gameW, int gameH) {
    g_shmFd = (int)syscall(SYS_memfd_create, "tj_ipc", 0);   // no CLOEXEC: the child needs it
    if (g_shmFd < 0) { LOGE("memfd_create failed errno=%d", errno); return false; }
    if (ftruncate(g_shmFd, tj::ipc::kRegionBytes) != 0) { LOGE("shm ftruncate failed"); return false; }
    void* base = mmap(nullptr, tj::ipc::kRegionBytes, PROT_READ | PROT_WRITE, MAP_SHARED, g_shmFd, 0);
    if (base == MAP_FAILED) { LOGE("shm mmap failed"); return false; }
    memset(base, 0, tj::ipc::kHeaderBytes);
    g_hdr = new (base) tj::ipc::Header{};
    g_hdr->magic = tj::ipc::kMagic;
    g_hdr->version = tj::ipc::kVersion;
    g_hdr->surfW = (uint32_t)gameW;
    g_hdr->surfH = (uint32_t)gameH;
    g_hdr->ringBytes = tj::ipc::kRingBytes;
    g_ring = (uint8_t*)base + tj::ipc::kHeaderBytes;
    return true;
}

// The game binary ships as lib/arm64-v8a/libtjgame.so — same dir as this library, the one
// app-owned path Android allows exec from. dladdr on our own code finds that dir.
bool GameBinaryPath(char* out, size_t cap) {
    Dl_info info{};
    if (!dladdr((void*)&RedirectStdio, &info) || !info.dli_fname) return false;
    strncpy(out, info.dli_fname, cap - 1); out[cap - 1] = 0;
    char* slash = strrchr(out, '/');
    if (!slash) return false;
    snprintf(slash + 1, cap - (size_t)(slash + 1 - out), "libtjgame.so");
    return true;
}

std::atomic<bool> g_stop{false};                 // defined early: ReapThread uses it
ANativeActivity* g_activity = nullptr;

void* ReapThread(void*) {
    int st = 0;
    pid_t pid = g_child;
    pid_t p = waitpid(pid, &st, 0);
    if (p == pid) {
        if (WIFEXITED(st))        LOGI("game subprocess exited (code %d)", WEXITSTATUS(st));
        else if (WIFSIGNALED(st)) LOGE("game subprocess KILLED by signal %d", WTERMSIG(st));
    }
    // The game is gone. Without this, the app would show the last frame forever with input
    // silently dropped — indistinguishable from a hang. Finish the activity instead (the
    // destroy path then exit(0)s, so a relaunch starts clean). Deliberate teardown
    // (KillGame set g_stop first) skips the finish — the activity is already going down.
    if (!g_stop.load(std::memory_order_acquire)) {
        g_stop.store(true, std::memory_order_release);
        if (g_hdr) g_hdr->quit.store(1, std::memory_order_release);
        if (g_activity) ANativeActivity_finish(g_activity);
    }
    return nullptr;
}

// ---------------------------------------------------------------- first-run game data
//
// A self-contained APK carries the player's OWN game files, packed by the installer out of
// their own disc image. They arrive as ONE uncompressed asset, `assets/game.pak`, and are
// unpacked here on first launch into the same <external>/extracted/ tree the app has always
// read. After that the app is byte-for-byte in the state a manual `adb push` would have left
// it, so nothing downstream — not the game, not the LAN data hash — can tell the difference.
//
// WHY ONE PACK FILE INSTEAD OF LOOSE ASSETS. Two reasons, both practical:
//   * AAssetManager cannot reliably enumerate directories (AAssetDir lists files, not
//     subdirectories), so walking a 450-file tree of loose assets needs an index anyway;
//   * a pack lets the INSTALLER stay trivial: it appends one STORED (uncompressed) zip entry
//     and never needs a deflate implementation. Game textures and audio barely compress, so
//     this costs almost nothing in size.
//
// FORMAT (little-endian): "TJPK", u32 version=1, u32 fileCount,
//   then fileCount x { u16 pathLen, path bytes ('/'-separated, relative), u64 size },
//   then every file's bytes back to back in the same order.
//
// ⚠ The completeness marker is default.xbe existing at the END of a successful unpack, so a
// half-finished extraction (killed app, full disk) is retried rather than half-trusted: the
// pack is unpacked to a .part directory that is only renamed into place once every byte is
// written.
namespace {

bool WriteAll(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) { if (errno == EINTR) continue; return false; }
        p += w; n -= (size_t)w;
    }
    return true;
}

bool MakeDirsFor(const std::string& path) {
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] != '/') continue;
        std::string dir = path.substr(0, i);
        if (mkdir(dir.c_str(), 0770) != 0 && errno != EEXIST) return false;
    }
    return true;
}

void RemoveTree(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string sub = dir + "/" + e->d_name;
        struct stat st {};
        if (!stat(sub.c_str(), &st) && S_ISDIR(st.st_mode)) RemoveTree(sub);
        else unlink(sub.c_str());
    }
    closedir(d);
    rmdir(dir.c_str());
}

// Unpack assets/game.pak into <ext>/extracted/. Returns false only when the pack EXISTS but
// could not be unpacked; a missing pack is not an error (the dev flow pushes files by adb).
bool UnpackGameData(android_app* app, const char* ext) {
    AAssetManager* am = app->activity->assetManager;
    if (!am) return true;
    AAsset* pak = AAssetManager_open(am, "game.pak", AASSET_MODE_STREAMING);
    if (!pak) { LOGI("no packed game data in the apk (expecting pushed files)"); return true; }

    struct Guard { AAsset* a; ~Guard() { AAsset_close(a); } } guard{ pak };
    char magic[4] = {};
    uint32_t ver = 0, count = 0;
    if (AAsset_read(pak, magic, 4) != 4 || memcmp(magic, "TJPK", 4) != 0 ||
        AAsset_read(pak, &ver, 4) != 4 || ver != 1 ||
        AAsset_read(pak, &count, 4) != 4 || count == 0 || count > 100000) {
        LOGE("game.pak header is not valid — refusing to unpack");
        return false;
    }
    struct Ent { std::string path; uint64_t size; };
    std::vector<Ent> ents;
    ents.reserve(count);
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t len = 0;
        if (AAsset_read(pak, &len, 2) != 2 || len == 0 || len > 512) { LOGE("game.pak index"); return false; }
        std::string path((size_t)len, '\0');
        if (AAsset_read(pak, &path[0], len) != (int)len) { LOGE("game.pak index"); return false; }
        uint64_t sz = 0;
        if (AAsset_read(pak, &sz, 8) != 8) { LOGE("game.pak index"); return false; }
        if (path.find("..") != std::string::npos || path[0] == '/') { LOGE("game.pak path"); return false; }
        ents.push_back({ path, sz });
        total += sz;
    }

    const std::string dest = std::string(ext) + "/extracted";
    const std::string part = dest + ".part";
    RemoveTree(part);
    LOGI("unpacking %u game files (%llu MB) — first launch only",
         count, (unsigned long long)(total >> 20));

    std::vector<uint8_t> buf(1u << 20);
    uint64_t done = 0;
    int lastPct = -1;
    for (const Ent& e : ents) {
        std::string out = part + "/" + e.path;
        if (!MakeDirsFor(out)) { LOGE("mkdir failed for %s", out.c_str()); RemoveTree(part); return false; }
        int fd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0660);
        if (fd < 0) { LOGE("create failed for %s (errno %d)", out.c_str(), errno); RemoveTree(part); return false; }
        uint64_t left = e.size;
        bool ok = true;
        while (left) {
            int want = (int)(left < buf.size() ? left : buf.size());
            int got = AAsset_read(pak, buf.data(), (size_t)want);
            if (got != want || !WriteAll(fd, buf.data(), (size_t)got)) { ok = false; break; }
            left -= (uint64_t)got;
            done += (uint64_t)got;
            int pct = total ? (int)(done * 100 / total) : 100;
            if (pct != lastPct && pct % 10 == 0) { LOGI("unpacking %d%%", pct); lastPct = pct; }
        }
        close(fd);
        if (!ok) { LOGE("short write unpacking %s", e.path.c_str()); RemoveTree(part); return false; }
    }

    RemoveTree(dest);
    if (rename(part.c_str(), dest.c_str()) != 0) {
        LOGE("could not move the unpacked data into place (errno %d)", errno);
        RemoveTree(part);
        return false;
    }
    LOGI("game data unpacked to %s", dest.c_str());
    return true;
}

// TOP UP an existing extraction. The full unpack is all-or-nothing and only runs when the
// tree is absent, so a NEW file added to game.pak by a later build -- arabic_font.bin was the
// first -- would never reach a phone that already had the game. The package manager replaces
// the .so on upgrade, so the code moved and the data did not, and the new build looked
// identical to the old one.
//
// game.pak is a STORED asset, so seeking is free: every entry that already matches is skipped
// without reading it, and an upgrade writes only what actually changed.
bool TopUpGameData(android_app* app, const char* ext) {
    AAssetManager* am = app->activity->assetManager;
    AAsset* pak = AAssetManager_open(am, "game.pak", AASSET_MODE_RANDOM);
    if (!pak) return true;                       // dev flow (adb push): nothing to top up
    char magic[4]; uint32_t ver = 0, count = 0;
    if (AAsset_read(pak, magic, 4) != 4 || memcmp(magic, "TJPK", 4) != 0 ||
        AAsset_read(pak, &ver, 4) != 4 || ver != 1 ||
        AAsset_read(pak, &count, 4) != 4 || count == 0 || count > 100000) {
        AAsset_close(pak); return true;          // not ours to judge here; the full path logs
    }
    struct Ent { std::string path; uint64_t size; uint64_t off; };
    std::vector<Ent> ents;
    ents.reserve(count);
    uint64_t running = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t len = 0; uint64_t sz = 0;
        if (AAsset_read(pak, &len, 2) != 2 || len == 0 || len > 512) { AAsset_close(pak); return true; }
        std::string path(len, 0);
        if (AAsset_read(pak, &path[0], len) != (int)len) { AAsset_close(pak); return true; }
        if (AAsset_read(pak, &sz, 8) != 8) { AAsset_close(pak); return true; }
        if (path.find("..") != std::string::npos || path[0] == '/') { AAsset_close(pak); return true; }
        Ent e; e.path = path; e.size = sz; e.off = running; running += sz;
        ents.push_back(e);
    }
    const off64_t dataBase = AAsset_seek64(pak, 0, SEEK_CUR);   // index consumed: data starts here
    const std::string dest = std::string(ext) + "/extracted";
    std::vector<uint8_t> buf(1u << 20);
    int wrote = 0;
    for (const Ent& e : ents) {
        const std::string out = dest + "/" + e.path;
        struct stat st {};
        if (stat(out.c_str(), &st) == 0 && (uint64_t)st.st_size == e.size) continue;  // current
        if (AAsset_seek64(pak, dataBase + (off64_t)e.off, SEEK_SET) < 0) break;
        if (!MakeDirsFor(out)) { LOGE("top-up mkdir failed for %s", out.c_str()); continue; }
        int fd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0660);
        if (fd < 0) { LOGE("top-up create failed for %s (errno %d)", out.c_str(), errno); continue; }
        uint64_t left = e.size; bool ok = true;
        while (left) {
            int want = (int)(left < buf.size() ? left : buf.size());
            int got = AAsset_read(pak, buf.data(), (size_t)want);
            if (got != want || !WriteAll(fd, buf.data(), (size_t)got)) { ok = false; break; }
            left -= (uint64_t)got;
        }
        close(fd);
        if (!ok) { LOGE("top-up short write for %s", e.path.c_str()); unlink(out.c_str()); continue; }
        LOGI("topped up %s (%llu bytes)", e.path.c_str(), (unsigned long long)e.size);
        ++wrote;
    }
    AAsset_close(pak);
    if (wrote) LOGI("game data topped up: %d file(s) added or replaced", wrote);
    return true;
}

// default.xbe present == the data is there. Cheap, and it is the file the game is handed.
bool GameDataPresent(const char* ext) {
    std::string xbe = std::string(ext) + "/extracted/default.xbe";
    struct stat st {};
    return stat(xbe.c_str(), &st) == 0 && st.st_size > 0;
}

} // namespace

bool SpawnGame(android_app* app) {
    char bin[512];
    if (!GameBinaryPath(bin, sizeof bin)) { LOGE("cannot locate libtjgame.so"); return false; }
    const char* ext = app->activity->externalDataPath;
    const char* internal = app->activity->internalDataPath;
    char xbe[512], shmFd[32], tw[16], th[16], lad[512];
    snprintf(xbe, sizeof xbe, "%s/extracted/default.xbe", ext ? ext : ".");
    snprintf(shmFd, sizeof shmFd, "TJ_SHM_FD=%d", g_shmFd);
    snprintf(tw, sizeof tw, "TJ_W=%d", g_gameW);
    snprintf(th, sizeof th, "TJ_H=%d", g_gameH);
    // LOCALAPPDATA -> internal files dir: file_io's UserDataDir() puts saves at
    // <internal>/TomJerryWOW/_SAVES — §2.8's internal-storage save home, structurally
    // separate from the external extraction target, with NO game-side changes.
    snprintf(lad, sizeof lad, "LOCALAPPDATA=%s", internal ? internal : ".");

    // OPTIONAL FLAGS FILE: <external>/tj_flags.txt, one KEY=VALUE per line (# comments,
    // blank lines ignored), merged into the game's environment. The engine's switches are
    // all env-driven (TJ_ENG_JIT, TJ_ENG_FAST, TJ_ENG_PROF2, TJ_ENG_NOCACHE...), but an
    // exec'd child of an APK has no shell to set them from — so an on-device A/B would
    // otherwise need a rebuild per leg. One `adb push` per leg instead. Absent file =
    // nothing added, which is the shipping path.
    static char flagBuf[2048];
    char* flagEnv[24];
    int nFlags = 0;
    {
        char fp[512];
        snprintf(fp, sizeof fp, "%s/tj_flags.txt", ext ? ext : ".");
        if (FILE* f = fopen(fp, "r")) {
            size_t n = fread(flagBuf, 1, sizeof flagBuf - 1, f);
            fclose(f);
            flagBuf[n] = 0;
            char* save = nullptr;
            for (char* ln = strtok_r(flagBuf, "\r\n", &save);
                 ln && nFlags < 24; ln = strtok_r(nullptr, "\r\n", &save)) {
                while (*ln == ' ' || *ln == '\t') ++ln;
                if (*ln == '#' || !*ln || !strchr(ln, '=')) continue;
                LOGI("flag: %s", ln);
                flagEnv[nFlags++] = ln;
            }
        }
    }

    int nEnv = 0; while (environ[nEnv]) ++nEnv;
    char** envp = (char**)malloc(sizeof(char*) * (size_t)(nEnv + 7 + nFlags));
    int k = 0;
    for (int i = 0; i < nEnv; ++i) envp[k++] = environ[i];
    envp[k++] = shmFd; envp[k++] = tw; envp[k++] = th; envp[k++] = lad;
    envp[k++] = (char*)"TJ_FE_NOVID=1";       // no PC video menu on Android (§2.7)
    envp[k++] = (char*)"TJ_FE_NOROW=1";
    for (int i = 0; i < nFlags; ++i) envp[k++] = flagEnv[i];   // last wins: overrides above
    envp[k] = nullptr;
    char* argv[3] = { bin, xbe, nullptr };

    // Self-contained APK: unpack the player's own game files on first launch. A dev build
    // whose files were pushed by adb already has them, and skips straight through.
    // Present? top it up (an upgrade may add files). Absent? full unpack.
    if (GameDataPresent(ext ? ext : ".")) TopUpGameData(app, ext ? ext : ".");
    else if (!UnpackGameData(app, ext ? ext : "."))
        LOGE("game data could not be unpacked — the game will fail to find default.xbe");

    LOGI("spawning game: %s %s (game res %dx%d)", bin, xbe, g_gameW, g_gameH);
    pid_t pid = fork();
    if (pid == 0) {                    // child: exec immediately (fork+exec is the safe
        execve(bin, argv, envp);       // shape in an ART process)
        _exit(127);
    }
    free(envp);
    if (pid < 0) { LOGE("fork failed errno=%d", errno); return false; }
    g_child = pid;
    pthread_t t;
    if (pthread_create(&t, nullptr, &ReapThread, nullptr) == 0) pthread_detach(t);
    return true;
}

void KillGame() {
    if (!g_child) return;
    if (g_hdr) g_hdr->quit.store(1, std::memory_order_release);
    for (int i = 0; i < 30; ++i) {                 // ~300 ms of grace for a clean exit
        // != 0 covers BOTH "exited now" (== pid) and "already reaped by ReapThread"
        // (-1 ECHILD). Only rc == 0 means still running. Treating ECHILD as running
        // would time out and SIGKILL a possibly-RECYCLED pid — never risk that.
        if (waitpid(g_child, nullptr, WNOHANG) != 0) { g_child = 0; return; }
        usleep(10000);
    }
    kill(g_child, SIGKILL);
    waitpid(g_child, nullptr, 0);                  // may be -1 ECHILD (reaper won) — fine
    g_child = 0;
}

// ---- input (app -> shm, seqlocked) ---------------------------------------------------------
struct Pad {
    unsigned short buttons = 0;
    unsigned char  analog[8] = {0};
    short lx = 0, ly = 0, rx = 0, ry = 0;
};
Pad g_gamepad[tj::ipc::kMaxPads], g_touch;
// DEVICE -> SEAT. Android hands every input event a device id; without routing them, all of
// them landed in one Pad and only player 1 ever existed. A device claims the lowest free seat
// THE FIRST TIME SOMEBODY ACTUALLY PRESSES SOMETHING ON IT -- not merely by existing. That is
// the rule the PC path already learned the hard way: idle virtual pads (vendor software,
// wireless dongles) enumerate themselves and would otherwise silently take player 1's seat,
// and on Android a resting controller emits centering motion events all on its own.
int32_t g_padDev[tj::ipc::kMaxPads] = { -1, -1, -1, -1 };
// THE TRIGGERS GET THEIR OWN SLOT, and it is not fussiness. Android pads report a trigger in
// one of two ways: as an ANALOG AXIS on a motion event, or as a DIGITAL KEYCODE
// (BUTTON_L2/R2). Both are written here, but they arrive on different event streams -- and a
// motion event fires for every stick nudge. If the axis path wrote straight into
// g_gamepad.analog[], a pad that reports its triggers only as keycodes would have a held
// block CLEARED by the player moving the stick, which is the whole time in a fighting game.
// Keeping them apart and merging by max in PushPad means neither source can erase the other.
uint8_t g_axisTrig[tj::ipc::kMaxPads][2];   // per seat: [0] = LT, [1] = RT, 0..255

// Resolve a device to its seat. `claim` is true only for events that represent a deliberate
// press; a resting device never takes a seat. Returns -1 when the device has no seat yet.
int SeatFor(int32_t dev, bool claim) {
    for (int i = 0; i < tj::ipc::kMaxPads; ++i) if (g_padDev[i] == dev) return i;
    if (!claim) return -1;
    for (int i = 0; i < tj::ipc::kMaxPads; ++i) {
        if (g_padDev[i] < 0) {
            g_padDev[i] = dev;
            LOGI("pad: device %d -> player %d", dev, i + 1);
            return i;
        }
    }
    return -1;                                  // five controllers, four seats
}
int g_surfW = 0, g_surfH = 0;         // ACTUAL current surface dims (touch-zone space)

void PushPad() {
    if (!g_hdr) return;
    tj::ipc::PadShm p[tj::ipc::kMaxPads]{};
    for (int s = 0; s < tj::ipc::kMaxPads; ++s) {
        const Pad& g = g_gamepad[s];
        // The TOUCH overlay is player 1's and only player 1's -- it is one set of controls on
        // one screen, so merging it into every seat would make one tap move all four fighters.
        const Pad& t = (s == 0) ? g_touch : Pad{};
        p[s].buttons = (uint16_t)(g.buttons | t.buttons);
        for (int i = 0; i < 8; ++i)
            p[s].analog[i] = g.analog[i] > t.analog[i] ? g.analog[i] : t.analog[i];
        if (g_axisTrig[s][0] > p[s].analog[6]) p[s].analog[6] = g_axisTrig[s][0];
        if (g_axisTrig[s][1] > p[s].analog[7]) p[s].analog[7] = g_axisTrig[s][1];
        p[s].lx = t.lx ? t.lx : g.lx;
        p[s].ly = t.ly ? t.ly : g.ly;
        p[s].rx = t.rx ? t.rx : g.rx;
        p[s].ry = t.ry ? t.ry : g.ry;
    }
    // Seqlock write (single writer — the glue thread). The RELEASE FENCE after the odd
    // bump is load-bearing: a release *increment* alone orders prior stores, not the pad
    // stores that FOLLOW it, so without the fence arm64 may publish new pad bytes while
    // padSeq still reads even — a torn snapshot the reader would accept. (Linux's
    // write_seqcount_begin is exactly seq++; smp_wmb().)
    g_hdr->padSeq.fetch_add(1, std::memory_order_relaxed);      // -> odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    for (int s = 0; s < tj::ipc::kMaxPads; ++s) g_hdr->pad[s] = p[s];
    g_hdr->padSeq.fetch_add(1, std::memory_order_release);      // -> even: stable
}

void ClassifyTouch(float x, float y, Pad& t) {
    if (g_surfW <= 0 || g_surfH <= 0) return;
    if (x < g_surfW / 3.0f) {
        float cx = g_surfW / 6.0f, cy = g_surfH * 0.62f;
        float span = g_surfH * 0.22f;
        float nx = (x - cx) / span, ny = (y - cy) / span;
        if (nx > 1) nx = 1; if (nx < -1) nx = -1;
        if (ny > 1) ny = 1; if (ny < -1) ny = -1;
        t.lx = (short)(nx * 32000);
        t.ly = (short)(-ny * 32000);              // screen y is down; stick y is up
    } else if (x > g_surfW * 0.66f) {
        bool right = x > g_surfW * 0.83f;
        bool bottom = y > g_surfH * 0.55f;
        bool top = y < g_surfH * 0.25f;
        if (top && right) t.buttons |= 0x10;            // START
        else if (bottom && right) t.analog[0] = 255;    // A
        else if (bottom && !right) t.analog[2] = 255;   // X
        else t.analog[1] = 255;                         // B
    }
}

int32_t OnInput(android_app*, AInputEvent* e) {
    int32_t type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(e);
        int32_t act = AKeyEvent_getAction(e);
        if (act != AKEY_EVENT_ACTION_DOWN && act != AKEY_EVENT_ACTION_UP) return 0;
        bool down = (act == AKEY_EVENT_ACTION_DOWN);
        // A key DOWN is a deliberate press, so it may claim a seat; a key UP may not (a
        // release from a device we never saw pressed is not a player arriving).
        int seat = SeatFor(AInputEvent_getDeviceId(e), down);
        if (seat < 0) return 0;
        Pad& pad = g_gamepad[seat];
        auto btn = [&](unsigned short b) { if (down) pad.buttons |= b; else pad.buttons &= (unsigned short)~b; };
        auto ana = [&](int i) { pad.analog[i] = down ? 255 : 0; };
        switch (code) {
            case AKEYCODE_BUTTON_A: ana(0); break;                 // A jump / confirm
            case AKEYCODE_BUTTON_B: ana(1); break;                 // B grab / back
            case AKEYCODE_BUTTON_X: ana(2); break;                 // X kick
            case AKEYCODE_BUTTON_Y: ana(3); break;
            case AKEYCODE_BUTTON_L1: ana(5); break;                // White
            case AKEYCODE_BUTTON_R1: ana(4); break;                // Black
            case AKEYCODE_BUTTON_L2: ana(6); break;                // LT
            case AKEYCODE_BUTTON_R2: ana(7); break;                // RT -- BLOCK
            case AKEYCODE_BUTTON_START: case AKEYCODE_MENU: btn(0x10); break;
            case AKEYCODE_BUTTON_SELECT:                    btn(0x20); break;
            case AKEYCODE_DPAD_UP:    btn(0x01); break;
            case AKEYCODE_DPAD_DOWN:  btn(0x02); break;
            case AKEYCODE_DPAD_LEFT:  btn(0x04); break;
            case AKEYCODE_DPAD_RIGHT: btn(0x08); break;
            default: return 0;    // BACK/HOME/volume stay with the system
        }
        PushPad();
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t source = AInputEvent_getSource(e);
        if (source & AINPUT_SOURCE_JOYSTICK) {
            // BEFORE any seat resolution: this one-shot exists to prove joystick events reach
            // us at all, and a device that has not claimed a seat yet is exactly the case it
            // has to be able to report. Behind the seat guard it could never fire.
            static bool firstSeen = false;
            if (!firstSeen) {
                firstSeen = true;
                LOGI("pad: joystick events arriving (device %d)", AInputEvent_getDeviceId(e));
            }
            float ax = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
            float ay = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
            float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
            float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
            // ⚠ THE TRIGGERS WERE NEVER READ AT ALL, on either path. analog[6]/[7] (LT/RT)
            // simply stayed 0 on Android for every pad, so BLOCK -- which is RT -- could not
            // be pressed, while every other control worked. The Windows path fills all eight
            // analog bytes from XInput; this one filled six.
            // Pads disagree about WHICH axis carries a trigger: the documented Android
            // mapping is LTRIGGER/RTRIGGER, but many pads (and Android's own automotive-
            // derived naming) report the same physical pull on BRAKE/GAS, and some report
            // both. Taking the larger of each pair covers every pad without a device table,
            // and cannot misfire: an unreported axis reads 0.
            auto trig = [&](int32_t a, int32_t b) -> uint8_t {
                float v = AMotionEvent_getAxisValue(e, a, 0);
                float w = AMotionEvent_getAxisValue(e, b, 0);
                if (w > v) v = w;
                if (v <= 0.0f) return 0;
                if (v >= 1.0f) return 255;
                return (uint8_t)(v * 255.0f + 0.5f);
            };
            // A stick or trigger genuinely deflected is a deliberate press and may claim a
            // seat; a resting controller emitting centering events may not.
            uint8_t lt = trig(AMOTION_EVENT_AXIS_LTRIGGER, AMOTION_EVENT_AXIS_BRAKE);
            uint8_t rt = trig(AMOTION_EVENT_AXIS_RTRIGGER, AMOTION_EVENT_AXIS_GAS);
            bool moved = (ax < -0.3f || ax > 0.3f || ay < -0.3f || ay > 0.3f ||
                          hx < -0.5f || hx > 0.5f || hy < -0.5f || hy > 0.5f ||
                          lt > 40 || rt > 40);
            int seat = SeatFor(AInputEvent_getDeviceId(e), moved);
            if (seat < 0) return 0;
            Pad& pad = g_gamepad[seat];
            g_axisTrig[seat][0] = lt;
            g_axisTrig[seat][1] = rt;
            // ONE LINE, ONCE, NAMING WHAT THIS PAD ACTUALLY SENDS. The trigger bug above was
            // invisible from the outside -- every other control worked, so the report could
            // only ever be "block does not work" with nothing to act on. If a pad still fails
            // after this, `adb logcat -s wotw` says in one line which axes it moves, and
            // whether the value arrives at all, instead of costing another round trip.
            // TWO one-shots, because there are exactly two ways this can still fail and they
            // need different answers: no joystick events reach us at all, or they do but the
            // pad puts its triggers on an axis we are not reading. The second line fires the
            // first time ANY trigger candidate leaves rest -- i.e. when the player actually
            // pulls one -- which is the moment that names the axis.
            {
                float lta = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0);
                float brk = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0);
                float rta = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
                float gas = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
                float za  = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0);
                float rza = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0);
                static bool pullSeen = false;
                if (!pullSeen && (lta > 0.1f || brk > 0.1f || rta > 0.1f || gas > 0.1f ||
                                  za > 0.1f || rza > 0.1f)) {
                    pullSeen = true;
                    LOGI("pad trigger axes (player %d): LTRIG=%.2f BRAKE=%.2f RTRIG=%.2f "
                         "GAS=%.2f Z=%.2f RZ=%.2f -> LT=%u RT=%u",
                         seat + 1, lta, brk, rta, gas, za, rza,
                         g_axisTrig[seat][0], g_axisTrig[seat][1]);
                }
            }
            pad.lx = (short)(ax * 32000);
            pad.ly = (short)(-ay * 32000);
            pad.buttons &= (unsigned short)~0x0Fu;
            if (hx < -0.5f) pad.buttons |= 0x04; else if (hx > 0.5f) pad.buttons |= 0x08;
            if (hy < -0.5f) pad.buttons |= 0x01; else if (hy > 0.5f) pad.buttons |= 0x02;
            PushPad();
            return 1;
        }
        if (source & AINPUT_SOURCE_TOUCHSCREEN) {
            int32_t action = AMotionEvent_getAction(e);
            int32_t flags = action & AMOTION_EVENT_ACTION_MASK;
            int upIdx = -1;
            if (flags == AMOTION_EVENT_ACTION_POINTER_UP)
                upIdx = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                        >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
            Pad t{};
            if (flags != AMOTION_EVENT_ACTION_UP && flags != AMOTION_EVENT_ACTION_CANCEL) {
                int n = (int)AMotionEvent_getPointerCount(e);
                for (int i = 0; i < n; ++i) {
                    if (i == upIdx) continue;
                    ClassifyTouch(AMotionEvent_getX(e, i), AMotionEvent_getY(e, i), t);
                }
            }
            g_touch = t;
            PushPad();
            return 1;
        }
    }
    return 0;
}

// ---- audio: drain the game's shm PCM ring to the speaker (AAudio) --------------------------
// The game-side software mixer (mix_snd.cpp) produces 48 kHz stereo int16 frames into the
// region's audio ring; this callback is the consumer. Underrun plays silence — the mixer
// keeps real-time cadence regardless, so the game's audio state machine never notices.
AAudioStream* g_astream = nullptr;

aaudio_data_callback_result_t AudioCb(AAudioStream*, void*, void* audioData, int32_t numFrames) {
    int16_t* out = (int16_t*)audioData;
    if (!g_hdr) { memset(out, 0, (size_t)numFrames * 4); return AAUDIO_CALLBACK_RESULT_CONTINUE; }
    int16_t* ring = (int16_t*)((uint8_t*)g_hdr + tj::ipc::kHeaderBytes + tj::ipc::kRingBytes);
    uint64_t tail = g_hdr->audioTail.load(std::memory_order_relaxed);
    uint64_t head = g_hdr->audioHead.load(std::memory_order_acquire);
    for (int32_t i = 0; i < numFrames; ++i) {
        if (tail < head) {
            uint32_t s = (uint32_t)(tail % tj::ipc::kAudioRingFrames);
            out[i * 2]     = ring[s * 2];
            out[i * 2 + 1] = ring[s * 2 + 1];
            ++tail;
        } else {
            out[i * 2] = out[i * 2 + 1] = 0;
        }
    }
    g_hdr->audioTail.store(tail, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void StartAudio() {
    if (g_astream) { AAudioStream_requestStart(g_astream); return; }
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) { LOGE("aaudio builder failed"); return; }
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)tj::ipc::kAudioChannels);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)tj::ipc::kAudioRate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, &AudioCb, nullptr);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_astream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !g_astream) { LOGE("aaudio open failed (%d)", (int)r); g_astream = nullptr; return; }
    AAudioStream_requestStart(g_astream);
    LOGI("audio out: %d Hz x%d (AAudio)", AAudioStream_getSampleRate(g_astream),
         AAudioStream_getChannelCount(g_astream));
}
void PauseAudio() { if (g_astream) AAudioStream_requestPause(g_astream); }

// ---- render thread: replay the ring into the gles device -----------------------------------
std::atomic<ANativeWindow*> g_pendingWindow{nullptr};   // glue -> render: new window arrived
std::atomic<bool> g_surfaceLost{false};                 // glue -> render: release the surface
std::atomic<bool> g_surfaceDown{true};                  // render -> glue: released, safe to return

tj::gfx::Device g_dev;
int g_texMap[4096];
bool     g_compProf = false;                  // TJ_COMP_PROF=1 in the flags file
// Catch-up frame dropping (TJ_COMP_DROP=0 disables). ON by default because the alternative
// is worse on exactly the hardware that needs help: this thread paces the simulation, so a
// compositor that misses the budget slows the GAME down instead of dropping a picture.
bool     g_compDrop = true;
uint64_t g_dropped = 0;                       // superseded frames whose draws were skipped
// TJ_GAME_SCALE=<f> (0.35..1.0): render the game at a fraction of the 16:9 rect, scaled up
// on present. Two purposes: it is the decisive CPU-vs-GPU experiment for the replay cost
// (fill rate scales with it, driver call count does not), and if the display leg is
// fill-bound it is also the fix — a 2002 game's art does not need every phone pixel.
float    g_gameScale = 1.0f;
uint32_t g_compDiag = 0;                      // TJ_COMP_DIAG=<mask>, see gles_gfx.cpp
uint64_t g_clsNs[4] = {}, g_clsN[4] = {};     // draw / texture / rendertarget / state

// ---- ONE BUFFER UPLOAD PER FRAME ---------------------------------------------------------
// The measurement that motivates this is at the top of the draw section in gles_gfx.cpp: the
// per-draw vertex glBufferData was 5.44 ms of a 6.99 ms in-match draw budget at 253 draws.
//
// The ring already IS the frame command buffer -- in order, with every vertex byte inline. So
// a frame is walked TWICE. Pass 1 copies each draw geometry into two CPU staging blocks and
// remembers the byte offset it landed at; the two blocks go up in ONE glBufferData each; pass
// 2 replays the commands in their exact original order, each draw naming its slice by offset.
// Command ORDER -- every Clear, SetTexture, render-target switch, blend/depth change -- is
// untouched by construction, because pass 2 is the same switch the streaming path always ran.
struct DrawSlice { uint32_t vbOfs, ibOfs; };
std::vector<uint8_t>   g_vbStage, g_ibStage;
std::vector<DrawSlice> g_slices;
bool g_batch = true;                        // TJ_COMP_BATCH=0 reverts to per-draw uploads

inline uint32_t StageAppend(std::vector<uint8_t>& st, const void* src, size_t n, size_t align) {
    size_t ofs = (st.size() + (align - 1)) & ~(align - 1);
    st.resize(ofs + n);
    if (n) memcpy(st.data() + ofs, src, n);
    return (uint32_t)ofs;
}

// Cheap header-only walk: is a COMPLETE frame (one ending in OP_PRESENT) already in the ring?
// Asked before staging so a frame still arriving is never memcpy'd once per 1 ms poll.
bool HaveCompleteFrame(uint64_t tail, uint64_t head) {
    using namespace tj::ipc;
    const uint32_t N = g_hdr->ringBytes;
    while (tail < head) {
        uint32_t pos = (uint32_t)(tail % N);
        CmdHdr ch;
        memcpy(&ch, g_ring + pos, sizeof ch);
        if (ch.op == OP_WRAP) { tail += sizeof(CmdHdr) + ch.bytes; continue; }
        if (ch.op == OP_PRESENT) return true;
        tail += CmdTotal(ch.bytes);
    }
    return false;
}

// Pass 1. Stages the geometry of every draw up to and including the frame OP_PRESENT.
// Returns false if a draw header claims more bytes than its command carries -- a malformed
// stream must not be allowed to size a memcpy, and the caller falls back to the legacy path.
bool StageFrame(uint64_t tail, uint64_t head) {
    using namespace tj::ipc;
    const uint32_t N = g_hdr->ringBytes;
    g_vbStage.clear(); g_ibStage.clear(); g_slices.clear();
    while (tail < head) {
        uint32_t pos = (uint32_t)(tail % N);
        CmdHdr ch;
        memcpy(&ch, g_ring + pos, sizeof ch);
        if (ch.op == OP_WRAP) { tail += sizeof(CmdHdr) + ch.bytes; continue; }
        const uint8_t* pl = g_ring + pos + sizeof(CmdHdr);
        DrawSlice sl = { 0, 0 };
        switch (ch.op) {
            case OP_DRAW_PC: {
                uint32_t vc; memcpy(&vc, pl, 4);
                size_t vb = (size_t)vc * sizeof(tj::gfx::VertexPC);
                if (4 + vb > ch.bytes) return false;
                sl.vbOfs = StageAppend(g_vbStage, pl + 4, vb, 32);
                g_slices.push_back(sl);
            } break;
            case OP_DRAW_PTC: {
                uint32_t hd[2]; memcpy(hd, pl, 8);
                size_t vb = (size_t)hd[0] * sizeof(tj::gfx::VertexPTC), ib = (size_t)hd[1] * 2;
                if (8 + vb + ib > ch.bytes) return false;
                sl.vbOfs = StageAppend(g_vbStage, pl + 8, vb, 32);
                sl.ibOfs = StageAppend(g_ibStage, pl + 8 + vb, ib, 4);
                g_slices.push_back(sl);
            } break;
            case OP_DRAW_SHINY: {
                int32_t hd[6]; memcpy(hd, pl, 24);
                if (hd[0] < 0 || hd[1] < 0) return false;
                size_t vb = (size_t)hd[0] * sizeof(tj::gfx::VertexPT2C), ib = (size_t)hd[1] * 2;
                if (24 + vb + ib > ch.bytes) return false;
                sl.vbOfs = StageAppend(g_vbStage, pl + 24, vb, 32);
                sl.ibOfs = StageAppend(g_ibStage, pl + 24 + vb, ib, 4);
                g_slices.push_back(sl);
            } break;
            case OP_PRESENT: return true;
            default: break;
        }
        tail += CmdTotal(ch.bytes);
    }
    return false;                            // caller only calls this after HaveCompleteFrame
}

// Pass 2 (and, with slices == nullptr, the whole legacy streaming path). Executes commands
// until one OP_PRESENT has been consumed or the ring runs dry; the caller swaps.
// Returns the number of OP_PRESENTs consumed (-1 = stream desync).
//
// skipDraws: consume a frame WITHOUT issuing its draw calls. Used only for a frame that a
// newer complete frame has already superseded, and it is the difference between a slow GPU
// dropping frames and a slow GPU slowing the GAME DOWN: the sim is throttled by this thread
// (the producer blocks at two unconsumed frames, ipc_gfx.cpp Present), and the game advances
// a FIXED TIMESTEP per frame, so a compositor that cannot hold 16.67 ms drags the simulation
// into literal slow motion rather than dropping a picture. Skipping only the draws -- every
// resource, state, render-target and present command still executes in order -- means the
// dropped frame changes nothing a later frame can observe. It is not the session-30 "frame
// drop" that flickered: that one presented a PARTIALLY drained frame, whereas this consumes
// only frames that are already complete and always presents the newest one in full.
int ReplayRange(const DrawSlice* slices, bool skipDraws = false) {
    using namespace tj::ipc;
    const uint32_t N = g_hdr->ringBytes;
    uint64_t tail = g_hdr->tail.load(std::memory_order_relaxed);
    uint64_t head = g_hdr->head.load(std::memory_order_acquire);
    int presents = 0;
    size_t di = 0;                           // next slice: draws are replayed in stream order
    auto mapTex = [&](int32_t h) -> int {
        return (h >= 0 && h < 4096) ? g_texMap[h] : -1;
    };
    while (tail < head) {
        uint32_t pos = (uint32_t)(tail % N);
        CmdHdr ch;
        memcpy(&ch, g_ring + pos, sizeof ch);
        if (ch.op == OP_WRAP) { tail += sizeof(CmdHdr) + ch.bytes; g_hdr->tail.store(tail, std::memory_order_release); continue; }
        const uint8_t* pl = g_ring + pos + sizeof(CmdHdr);
        uint64_t opT0 = g_compProf ? NowNs() : 0;
        switch (ch.op) {
            case OP_CLEAR: {
                uint32_t v[4]; memcpy(v, pl, 16);
                float z; memcpy(&z, &v[2], 4);
                g_dev.Clear(v[0], v[1], z, (uint8_t)v[3]);
            } break;
            case OP_SET_TRANSFORM: {
                float m[16]; memcpy(m, pl, 64);
                g_dev.SetTransform(m);
            } break;
            case OP_DRAW_PC: {
                uint32_t vc; memcpy(&vc, pl, 4);
                if (skipDraws)   { }
                else if (slices) tj::gfx::GlesDrawPCAt(slices[di++].vbOfs, (int)vc);
                else             g_dev.DrawTriangleList((const tj::gfx::VertexPC*)(pl + 4), (int)vc);
            } break;
            case OP_DRAW_PTC: {
                uint32_t hd[2]; memcpy(hd, pl, 8);
                if (skipDraws) {
                } else if (slices) {
                    const DrawSlice& sl = slices[di++];
                    tj::gfx::GlesDrawPTCAt(sl.vbOfs, (int)hd[0], sl.ibOfs, (int)hd[1]);
                } else {
                    const auto* v = (const tj::gfx::VertexPTC*)(pl + 8);
                    const uint16_t* idx = (const uint16_t*)(pl + 8 + hd[0] * sizeof(tj::gfx::VertexPTC));
                    g_dev.DrawIndexed(v, (int)hd[0], idx, (int)hd[1]);
                }
            } break;
            case OP_DRAW_SHINY: {
                int32_t hd[6]; memcpy(hd, pl, 24);
                if (skipDraws) {
                } else if (slices) {
                    const DrawSlice& sl = slices[di++];
                    tj::gfx::GlesDrawShinyAt(sl.vbOfs, hd[0], sl.ibOfs, hd[1],
                                             mapTex(hd[2]), mapTex(hd[3]), mapTex(hd[4]), mapTex(hd[5]));
                } else {
                    const auto* v = (const tj::gfx::VertexPT2C*)(pl + 24);
                    const uint16_t* idx = (const uint16_t*)(pl + 24 + (uint32_t)hd[0] * sizeof(tj::gfx::VertexPT2C));
                    g_dev.DrawShinyIndexed(v, hd[0], idx, hd[1],
                                           mapTex(hd[2]), mapTex(hd[3]), mapTex(hd[4]), mapTex(hd[5]));
                }
            } break;
            case OP_CREATE_TEX: {
                int32_t hd[3]; memcpy(hd, pl, 12);
                g_texMap[hd[0]] = g_dev.CreateTexture((const uint32_t*)(pl + 12), hd[1], hd[2]);
            } break;
            case OP_UPDATE_TEX: {
                int32_t hd[4]; memcpy(hd, pl, 16);
                int t = mapTex(hd[0]);
                if (t >= 0) g_dev.UpdateTexture(t, (const uint32_t*)(pl + 16), hd[1], hd[2], hd[3] != 0);
            } break;
            case OP_DESTROY_TEX: {
                int32_t h; memcpy(&h, pl, 4);
                int t = mapTex(h);
                if (t >= 0) { g_dev.DestroyTexture(t); g_texMap[h] = -1; }
            } break;
            case OP_CREATE_RT: {
                int32_t hd[3]; memcpy(hd, pl, 12);
                g_texMap[hd[0]] = g_dev.CreateRenderTexture(hd[1], hd[2]);
            } break;
            case OP_SET_RT: {
                int32_t h; memcpy(&h, pl, 4);
                int t = mapTex(h);
                if (t >= 0) g_dev.SetRenderTexture(t);
            } break;
            case OP_SET_RT_BACKBUFFER: g_dev.SetRenderTargetBackbuffer(); break;
            case OP_CREATE_CAPTURE: {
                int32_t h; memcpy(&h, pl, 4);
                g_texMap[h] = g_dev.CreateCaptureTexture();
            } break;
            case OP_COPY_BACKBUFFER: {
                int32_t h; memcpy(&h, pl, 4);
                int t = mapTex(h);
                if (t >= 0) g_dev.CopyBackbufferTo(t);
            } break;
            case OP_SET_TEXTURE: {
                int32_t h; memcpy(&h, pl, 4);
                g_dev.SetTexture(h < 0 ? tj::gfx::kNoTexture : mapTex(h));
            } break;
            case OP_SET_UVCLAMP: {
                uint32_t v[2]; memcpy(v, pl, 8);
                g_dev.SetUvClamp(v[0] != 0, v[1] != 0);
            } break;
            case OP_SET_DEPTH: {
                uint32_t v; memcpy(&v, pl, 4);
                g_dev.SetDepthTest(v != 0);
            } break;
            case OP_SET_ALPHA: {
                uint32_t v; memcpy(&v, pl, 4);
                g_dev.SetAlphaBlend(v != 0);
            } break;
            case OP_SET_BLEND: {
                uint32_t v[2]; memcpy(v, pl, 8);
                g_dev.SetBlendMode((tj::gfx::Device::BlendMode)v[0], v[1] != 0);
            } break;
            case OP_PRESENT:
                // Counted, NOT swapped here — the caller swaps once per drained batch.
                g_hdr->framesConsumed.fetch_add(1, std::memory_order_acq_rel);
                ++presents;
                break;
            default:
                LOGE("replay: unknown op %u — stream desync, stopping", ch.op);
                g_hdr->quit.store(1, std::memory_order_release);   // or the child wedges in
                g_stop.store(true);                                // RingWrite backpressure
                return -1;
        }
        if (g_compProf) {
            uint64_t d = NowNs() - opT0;
            int cls = (ch.op == OP_DRAW_PC || ch.op == OP_DRAW_PTC || ch.op == OP_DRAW_SHINY) ? 0
                    : (ch.op == OP_CREATE_TEX || ch.op == OP_UPDATE_TEX) ? 1
                    : (ch.op == OP_CREATE_RT || ch.op == OP_CREATE_CAPTURE ||
                       ch.op == OP_COPY_BACKBUFFER || ch.op == OP_SET_RT ||
                       ch.op == OP_SET_RT_BACKBUFFER) ? 2 : 3;
            g_clsNs[cls] += d; ++g_clsN[cls];
        }
        tail += CmdTotal(ch.bytes);
        g_hdr->tail.store(tail, std::memory_order_release);   // free space promptly
        if (ch.op == OP_PRESENT) {
            // ONE present per swap. Draining several frames before swapping (the session-30
            // "frame drop") shows the display whatever the last drained frame left in the
            // backbuffer and is a second way to put a half-built picture on screen. The sim
            // is paced by its own 60 Hz limiter, not by this.
            break;
        }
    }
    return presents;
}

// Returns the number of OP_PRESENTs consumed (-1 = stream desync).
// Consume one ALREADY-COMPLETE frame without drawing it (see ReplayRange's skipDraws).
// Deliberately does not stage geometry: nothing will be drawn from it.
int ReplaySkipFrame() { return ReplayRange(nullptr, true); }

int ReplayRing() {
    if (g_batch) {
        uint64_t tail = g_hdr->tail.load(std::memory_order_relaxed);
        uint64_t head = g_hdr->head.load(std::memory_order_acquire);
        if (HaveCompleteFrame(tail, head)) {
            if (StageFrame(tail, head)) {
                tj::gfx::GlesStreamUpload(g_vbStage.data(), (unsigned long)g_vbStage.size(),
                                          g_ibStage.data(), (unsigned long)g_ibStage.size());
                return ReplayRange(g_slices.data());
            }
            LOGE("replay: malformed draw payload — falling back to per-draw uploads");
        } else if (head - tail < (uint64_t)(g_hdr->ringBytes / 2)) {
            return 0;                        // the frame is still arriving; wait for the rest
        }
        // No OP_PRESENT and a backlog past half the ring: a single frame can legitimately be
        // enormous (a level load emits every texture before it presents), and waiting for its
        // OP_PRESENT would deadlock the producer against a full ring. Drain it the old way.
    }
    return ReplayRange(nullptr);
}

void* RenderThread(void* pv) {
    ANativeWindow* win = (ANativeWindow*)pv;
    for (int i = 0; i < 4096; ++i) g_texMap[i] = -1;
    tj::gfx::GlesSetDrawDiag(g_compDiag);        // before Create: it sizes the diag buffers
    tj::gfx::SetAndroidDisplaySize(g_gameW, g_gameH);
    tj::gfx::GlesSetGameSize(g_gameW, g_gameH);   // 16:9 present rect, centered per attach
    tj::gfx::PresentParams pp;
    pp.backWidth = g_gameW; pp.backHeight = g_gameH;
    // vsync ON. It was turned OFF (session 30) to stop the swap blocking the replay
    // thread; it bought no measurable frame time and it TEARS — the user reported
    // flickering, which is what an unsynchronised swap looks like. Correct picture first.
    pp.vsync = true;
    for (;;) {
        if (g_stop.load()) return nullptr;
        // Service the TERM handshake even while no device exists: the glue thread is
        // waiting on g_surfaceDown, the framework frees the window when it returns, and
        // Create must never touch a freed one.
        if (g_surfaceLost.load(std::memory_order_acquire)) {
            win = nullptr;
            g_surfaceDown.store(true, std::memory_order_release);
            g_surfaceLost.store(false, std::memory_order_release);
        }
        ANativeWindow* nw = g_pendingWindow.exchange(nullptr, std::memory_order_acq_rel);
        if (nw) win = nw;
        if (!win) { usleep(20000); continue; }
        if (g_dev.Create(reinterpret_cast<HWND>(win), pp)) break;
        LOGE("gles device create failed; retrying");
        // Retrying the SAME window is safe: the framework only frees it after TERM_WINDOW
        // returns, and the handshake at the loop top nulls `win` when that happens.
        usleep(200000);
    }
    g_surfaceDown.store(false);
    LOGI("compositor up (game %dx%d)", g_gameW, g_gameH);
    bool haveSurface = true;
    // Per-400-present accounting. `wall` is what the session-30 measurement reported; `cpu`
    // is this thread's own CPU time over the same span. drain* covers the PARTIAL drains
    // (the calls that returned before an OP_PRESENT), which the old counter dropped
    // entirely — if the ring arrives in dribs the frame's replay is spread across them.
    uint64_t rN = 0, rReplay = 0, rSwap = 0, rBatch = 0;
    uint64_t rCpuReplay = 0, rCpuSwap = 0;
    uint64_t rPart = 0, rPartCpu = 0, rPartN = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        if (g_surfaceLost.load(std::memory_order_acquire)) {
            if (haveSurface) { tj::gfx::GlesReleaseWindowSurface(); haveSurface = false; }
            g_surfaceDown.store(true, std::memory_order_release);
            g_surfaceLost.store(false, std::memory_order_release);
        }
        ANativeWindow* nw = g_pendingWindow.exchange(nullptr, std::memory_order_acq_rel);
        if (nw) {
            if (tj::gfx::GlesAttachWindowSurface(nw)) { haveSurface = true; g_surfaceDown.store(false); }
            else LOGE("surface attach failed");
        }
        if (!haveSurface) { usleep(20000); continue; }   // ring backpressure pauses the game
        // Compositor cost, per 400 presented frames. The GAME's own `swap` figure is the
        // time it BLOCKS in Present, which conflates the 60 Hz limiter with waiting for
        // this thread — so the replay+swap split has to be measured on this side to know
        // whether the display leg or the sim is the frame's real bound.
        // CATCH UP BY DROPPING, NOT BY SLOWING THE GAME DOWN. framesProduced - framesConsumed
        // is the number of complete frames waiting; anything but the last of them is already
        // superseded, so its draws are wasted work whose only effect is to hold the producer
        // at its two-frame cap and stall the simulation. On hardware that comfortably makes
        // the budget this never fires (the producer is never more than one frame ahead), so
        // it costs the phone nothing; on a weak GPU it converts slow motion into dropped
        // frames, which is what a fixed-timestep game needs. TJ_COMP_DROP=0 restores the
        // strict one-present-per-frame behaviour.
        uint64_t t0 = NowNs(), c0 = NowCpuNs();
        if (g_compDrop) {
            for (int guard = 0; guard < 8; ++guard) {
                uint32_t prod = g_hdr->framesProduced.load(std::memory_order_acquire);
                uint32_t cons = g_hdr->framesConsumed.load(std::memory_order_acquire);
                if ((uint32_t)(prod - cons) <= 1) break;   // this is the newest: draw it
                if (ReplaySkipFrame() <= 0) break;         // ring dry or desync
                ++g_dropped;
            }
        }
        int frames = ReplayRing();
        uint64_t t1 = NowNs(), c1 = NowCpuNs();
        if (frames > 0) {
            g_dev.Present();                             // ONE swap per drained batch
            uint64_t t2 = NowNs(), c2 = NowCpuNs();
            rReplay += t1 - t0; rSwap += t2 - t1; rBatch += (uint64_t)frames;
            rCpuReplay += c1 - c0; rCpuSwap += c2 - c1;
            if (((++rN) % 400) == 0) {
                LOGI("comp: replay %.2f ms (cpu %.2f) swap %.2f ms (cpu %.2f) "
                     "partial %.2f ms (cpu %.2f, n=%.1f) batch %.2f frames/drain (per present) "
                     "dropped=%llu",
                     rReplay / 400.0 / 1e6, rCpuReplay / 400.0 / 1e6,
                     rSwap / 400.0 / 1e6, rCpuSwap / 400.0 / 1e6,
                     rPart / 400.0 / 1e6, rPartCpu / 400.0 / 1e6, rPartN / 400.0,
                     rBatch / 400.0, (unsigned long long)g_dropped);
                if (g_compProf) {
                    static const char* kCls[4] = { "draw", "tex", "rt", "state" };
                    for (int i = 0; i < 4; ++i) {
                        LOGI("comp:   %-5s %8.2f ms/present  n=%.1f/present",
                             kCls[i], g_clsNs[i] / 400.0 / 1e6, g_clsN[i] / 400.0);
                        g_clsNs[i] = g_clsN[i] = 0;
                    }
                }
                if (g_compDiag) {
                    uint64_t d[4]; uint64_t nd = tj::gfx::GlesDrawDiagDrain(d);
                    LOGI("comp:   draw phases: vb %.2f ib %.2f state %.2f draw %.2f ms/present"
                         "  (%.1f draws/present, %.2f us/draw total)",
                         d[0] / 400.0 / 1e6, d[1] / 400.0 / 1e6, d[2] / 400.0 / 1e6,
                         d[3] / 400.0 / 1e6, nd / 400.0,
                         nd ? (d[0] + d[1] + d[2] + d[3]) / (double)nd / 1e3 : 0.0);
                }
                rReplay = rSwap = rBatch = rCpuReplay = rCpuSwap = 0;
                rPart = rPartCpu = rPartN = 0;
            }
        }
        else if (frames == 0) {                          // nothing complete: idle briefly
            rPart += t1 - t0; rPartCpu += c1 - c0; ++rPartN;
            usleep(1000);
        }
    }
    return nullptr;
}

// ---- lifecycle -----------------------------------------------------------------------------
pthread_t g_renderThread;
bool g_running = false;

void OnCmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: {
            if (!app->window) break;
            int w = ANativeWindow_getWidth(app->window);
            int h = ANativeWindow_getHeight(app->window);
            g_surfW = w; g_surfH = h;
            LOGI("window %dx%d", w, h);
            if (!g_running) {
                // The game renders LANDSCAPE dims regardless of what shape the first
                // (possibly pre-rotation) surface reports — and CLAMPED TO 16:9: the game
                // can only produce 4:3 or anamorphic-16:9 content, so filling a ~20:9
                // panel would stretch everything ~25%. The compositor centers the 16:9
                // image (pillarboxed) via the gles present rect.
                int W = w > h ? w : h, H = w > h ? h : w;
                if (W * 9 > H * 16) W = (H * 16 / 9) & ~1;   // wider than 16:9 -> pillarbox
                else if (W * 9 < H * 16) H = (W * 9 / 16) & ~1;  // taller -> letterbox
                if (g_gameScale < 0.999f) {                  // render smaller, scale on present
                    W = ((int)((float)W * g_gameScale)) & ~1;
                    H = ((int)((float)H * g_gameScale)) & ~1;
                    LOGI("game render scale %.2f -> %dx%d", (double)g_gameScale, W, H);
                }
                g_gameW = W; g_gameH = H;
                if (!CreateShm(g_gameW, g_gameH)) break;
                if (!SpawnGame(app)) break;
                if (pthread_create(&g_renderThread, nullptr, &RenderThread, app->window) != 0) {
                    LOGE("render thread create failed — tearing down");
                    g_stop.store(true);
                    KillGame();
                    break;                       // g_running stays false; INIT can retry
                }
                g_running = true;
                StartAudio();
            } else {
                g_pendingWindow.store(app->window, std::memory_order_release);
                StartAudio();                    // resume after a TERM (backgrounded) pause
            }
        } break;
        case APP_CMD_TERM_WINDOW: {
            // The surface dies when this callback returns. Clear any queued pending window
            // FIRST — it can only be the dying one, and the render thread must never
            // attach to a freed ANativeWindow — then wait (bounded) for the render thread
            // to actually let go of the current surface.
            if (!g_running) break;
            PauseAudio();                        // backgrounded: no sound from a hidden app
            g_pendingWindow.store(nullptr, std::memory_order_release);
            g_surfaceDown.store(false, std::memory_order_release);
            g_surfaceLost.store(true, std::memory_order_release);
            for (int i = 0; i < 200 && !g_surfaceDown.load(std::memory_order_acquire); ++i)
                usleep(5000);
        } break;
        default: break;
    }
}

} // namespace

// Fullscreen: hide status+nav bars, immersive sticky (swipe reveals temporarily). A theme
// cannot hide the nav bar — this is the standard JNI call NativeActivity apps make.
static void MakeImmersive(android_app* app) {
    JavaVM* vm = app->activity->vm;
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    jobject activity = app->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jmethodID getWindow = env->GetMethodID(actCls, "getWindow", "()Landroid/view/Window;");
    jobject window = env->CallObjectMethod(activity, getWindow);
    jclass winCls = env->GetObjectClass(window);
    jmethodID getDecor = env->GetMethodID(winCls, "getDecorView", "()Landroid/view/View;");
    jobject decor = env->CallObjectMethod(window, getDecor);
    jclass viewCls = env->GetObjectClass(decor);
    jmethodID setVis = env->GetMethodID(viewCls, "setSystemUiVisibility", "(I)V");
    // FULLSCREEN | HIDE_NAVIGATION | IMMERSIVE_STICKY | LAYOUT_* (stable layout).
    const jint flags = 0x00000004 | 0x00000002 | 0x00001000 | 0x00000100 | 0x00000200 | 0x00000400;
    env->CallVoidMethod(decor, setVis, flags);
    if (env->ExceptionCheck()) env->ExceptionClear();
    vm->DetachCurrentThread();
}

void android_main(android_app* app) {
    RedirectStdio();
    {   // TJ_COMP_PROF=1 in <external>/tj_flags.txt turns on per-op replay accounting.
        char fp[512];
        const char* ex = app->activity->externalDataPath;
        snprintf(fp, sizeof fp, "%s/tj_flags.txt", ex ? ex : ".");
        if (FILE* f = fopen(fp, "r")) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                if (strstr(line, "TJ_COMP_PROF=1")) g_compProf = true;
                if (strstr(line, "TJ_COMP_DROP=0")) g_compDrop = false;
                if (const char* dg = strstr(line, "TJ_COMP_DIAG="))
                    g_compDiag = (uint32_t)strtoul(dg + 13, nullptr, 0);
                if (strstr(line, "TJ_COMP_BATCH=0")) g_batch = false;
                if (const char* sc = strstr(line, "TJ_GAME_SCALE=")) {
                    float v = (float)atof(sc + 14);
                    if (v >= 0.35f && v <= 1.0f) g_gameScale = v;
                }
            }
            fclose(f);
        }
    }
    app->onAppCmd = OnCmd;
    app->onInputEvent = OnInput;
    g_activity = app->activity;        // ReapThread finishes the activity on a game crash
    // The screen must not lock mid-match (the pad is often idle for seconds); FULLSCREEN
    // backs up the theme's status-bar hide at the window-flag level too.
    ANativeActivity_setWindowFlags(app->activity,
        AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    MakeImmersive(app);
    LOGI("android_main start (compositor shell)");
    for (;;) {
        int events;
        android_poll_source* src;
        while (ALooper_pollOnce(-1, nullptr, &events, (void**)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) {
                LOGI("destroy requested — stopping game");
                g_stop.store(true);
                KillGame();
                if (g_running) pthread_join(g_renderThread, nullptr);
                // _exit(0), deliberately — TWO reasons. (1) Android CACHES the empty
                // process after the activity dies; android_main would re-run in it with
                // every static above still holding torn-down state — a guaranteed black
                // screen on relaunch. (2) It must be _exit, NOT exit: exit() runs static
                // destructors while the framework's own threads (hwuiTask) are still
                // live, and the device crash log showed exactly that — FORTIFY abort on
                // a destroyed mutex in hwuiTask during teardown.
                _exit(0);
            }
        }
    }
}
