// Phase-2 LAN -- transport (stage 1c) + discovery/handshake/lobby session (stage 3).
//
// THE MODEL. Both peers run the same simulation. Each frame, every peer needs ALL
// players' controls for that frame; given identical inputs and identical starting state,
// identical code produces identical results (proved in stage 1: 57,239 frames with zero
// divergence across fighters, all 50 item slots and the spawn schedulers). So the wire
// carries only ~0x1A bytes per player per frame -- never positions, health or items.
//
// DELAY. A peer cannot know a remote player's input for the frame it is about to step, so
// inputs are scheduled D frames ahead: what you press now is played on frame N+D by
// everyone. D=2 at 60 Hz is ~33 ms of input lag, which hides typical LAN latency without
// rollback. Frames 0..D-1 are pre-seeded neutral so both peers can start immediately.
//
// STALLING. If a remote input has not arrived, NetTickBegin returns false and the caller
// skips the ENTIRE update -- the simulation freezes and the match clock cannot advance
// (the tick counter is bumped inside the update, before the mode dispatch). Render is
// skipped too: the render phase MUTATES simulation state (camera shake), so a peer that
// draws more frames than its opponent drifts (see net_sync.cpp).
//
// THREADING. Everything runs on the game thread: recv is non-blocking and is pumped both
// from the lockstep tick and (for lobby/discovery traffic, which must flow while the
// simulation is not armed) once per presented frame from the frontend tick. LAN_PLAN
// budgeted a dedicated net thread; a non-blocking socket polled twice per frame needs no
// locking and cannot stall the barrier, and the barrier is the one thing worth protecting.
//
// PORTS. The data socket takes the first free port in [27100..27107] so several instances
// can run on one PC; beacons are broadcast to that whole range (and to 127.0.0.1), which
// is what makes two windows on one machine discover each other without any extra config.
#include "net_lan.h"
#include "hybrid/arabic.h"
#include "hybrid/meat_rush.h"
#include "hybrid/xdk_patch.h"   // PatchSetFingerprint (the sim-contract hash)
#include "net_sync.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
// POSIX/Android (Stage 7): the same transport over BSD sockets. Every packet is built from
// the same #pragma pack(1) structs and the same protocol constants, so the WIRE BYTES are
// identical to the Windows build -- an Android peer and a PC peer speak the same protocol 6.
#include "hybrid/host_compat.h"      // the Win32 subset (CreateFileA, GetTickCount, *_s CRT)
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
// ---------------------------------------------------------------- winsock -> BSD shims
// Local by design (host_compat.h stays socket-free): exactly the Winsock surface this file
// uses, mapped 1:1 so the call sites below compile unchanged. Nothing here can change a
// wire byte.
typedef int SOCKET;
static const SOCKET INVALID_SOCKET = -1;
static inline int closesocket(SOCKET s) { return ::close(s); }
static inline int WSAGetLastError() { return errno; }
// Only ever called with FIONBIO; BSD spells that fcntl(O_NONBLOCK).
static inline int ioctlsocket(SOCKET s, long /*cmd == FIONBIO*/, u_long* enable) {
    int fl = ::fcntl(s, F_GETFL, 0);
    if (fl < 0) return -1;
    return ::fcntl(s, F_SETFL, *enable ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}
// host_compat.h carries the INT reader the settings screens use; the LAN player name needs
// the STRING reader. Parses the same [section] key=value shape host_compat.cpp's
// WritePrivateProfileStringA writes (section/key case-insensitive, value trimmed).
static DWORD GetPrivateProfileStringA(const char* section, const char* key, const char* def,
                                      char* out, DWORD cap, const char* path) {
    if (!out || !cap) return 0;
    strncpy_s(out, cap, def ? def : "", _TRUNCATE);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return (DWORD)strlen(out);
    char line[512];
    bool inSec = false;
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '[') {
            char* e = strchr(p, ']');
            inSec = e && (size_t)(e - p - 1) == strlen(section) &&
                    strncasecmp(p + 1, section, (size_t)(e - p - 1)) == 0;
            continue;
        }
        if (!inSec) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        size_t kl = (size_t)(eq - p);
        while (kl && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) --kl;
        if (kl != strlen(key) || strncasecmp(p, key, kl) != 0) continue;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == '\n' || v[vl - 1] == '\r' || v[vl - 1] == ' ')) --vl;
        if (vl >= cap) vl = cap - 1;
        memcpy(out, v, vl);
        out[vl] = 0;
        break;
    }
    fclose(f);
    return (DWORD)strlen(out);
}
#endif

namespace tj::hybrid {

// ---------------------------------------------------------------- protocol
static const uint32_t kMagic     = 0x314E4A54;   // 'TJN1'
// 5: PktLobby and PktStart both grew a MEAT RUSH byte. A peer on 4 must be REFUSED at the
// version check rather than misparse a shorter packet, so this bump is mandatory.
// 6: JOIN carries the number of players sitting at one PC and the reply grants a seat MASK, so
// two people can share a machine. Both packets grew, so an older peer must be refused at JOIN
// rather than left to misparse them.
// 7: CROSS-PLATFORM PLAY. buildHash stopped being a hash of the module's own bytes (which a
// Windows .dll and an Android .so can never agree on, so a PC and a phone were refused as
// "DIFFERENT VERSION" by construction) and became a SIM-CONTRACT hash — see ComputeHashes.
// The beacon and both JOIN packets also grew the advisory moduleHash/patchFp, so the meaning
// of an existing field changed AND the layout grew: a peer on 6 must be refused at the
// version check, where it gets a message that says what to do.
static const uint8_t  kProtoVer  = 7;
static const int      kBasePort  = 27100;
static const int      kPortSpan  = 8;
static const int      kRing      = 256;          // frames of input history (power of two)
static const int      kSlots     = 4;
static const int      kMaxGames  = 12;
static const uint32_t kBeaconMs  = 500;          // 2 Hz
static const uint32_t kGameTtlMs = 3000;         // a game disappears from the browser
static const uint32_t kPingMs    = 250;
static const uint32_t kRetryMs   = 100;          // reliable-packet resend
static const uint32_t kPeerTtlMs = 5000;

enum : uint8_t {
    P_BEACON = 0x01, P_PROBE = 0x02,
    P_JOIN_REQ = 0x10, P_JOIN_RSP = 0x11, P_LOBBY = 0x12, P_INTENT = 0x13,
    P_START = 0x20, P_READY = 0x21, P_GO = 0x22,
    P_INPUT = 0x30,
    P_PING = 0x50, P_PONG = 0x51,
    P_BYE = 0x60,
};

#pragma pack(push, 1)
struct Hdr { uint32_t magic; uint8_t ver, type; uint16_t pad; };

struct PktBeacon {
    Hdr h; uint32_t sessionId; uint32_t buildHash, dataHash;
    char host[16]; uint8_t players, maxPlayers, mode, arena, locked, inMatch; uint16_t rsv;
    uint32_t moduleHash, patchFp;      // proto 7, ADVISORY: never gates anything, see below
};
struct PktProbe { Hdr h; uint32_t nonce; };
// ⚠ THE TWO JOIN PACKETS MUST STAY READABLE BY AN OLDER BUILD, or a version mismatch has no
// way to announce itself. Both grew in protocol 6, and the receiver used to require its OWN
// size -- so a v5 peer's shorter packet was dropped before anything looked at it, and BOTH
// sides sat in silence: the joiner retried forever and the host never even saw a request.
// The new fields are appended at the END and the length is checked against the v5 size, so an
// old packet still parses far enough to be refused with a message the player can act on.
struct PktJoinReq {
    Hdr h; uint32_t sessionId, buildHash, dataHash, pwHash; char name[16];
    uint8_t players, rsv[3];       // how many people are sitting at THIS PC (proto 6)
    uint32_t moduleHash, patchFp;  // proto 7, ADVISORY
};
struct PktJoinRsp {
    Hdr h; uint32_t sessionId; uint8_t ok, nak, slot, players;
    uint32_t buildHash, dataHash;
    uint8_t slotMask, rsv[3];      // every seat granted to this PC, bit s = seat s (proto 6)
    uint32_t moduleHash, patchFp;  // proto 7, ADVISORY
};
// The old-size floors stay measured from the CURRENT struct so an older packet still parses
// far enough to be refused with a message. Proto 6 added 4 bytes, proto 7 another 8.
static const int kJoinReqV5 = (int)(sizeof(PktJoinReq) - 12);
static const int kJoinRspV5 = (int)(sizeof(PktJoinRsp) - 12);
struct PktLobby {
    Hdr h; uint32_t sessionId, lobbyVer;
    uint8_t players, mode, arena, hostReadyMask;
    uint8_t kind[4], charId[4], costume[4], team[4], ready[4];
    char    name[4][16];
    uint32_t timeLimit; uint8_t rounds, difficulty, tournFirstTo, inputDelay;
    uint8_t  meat;                       // MEAT RUSH setting (blob byte 0x16A26B)
};
struct PktIntent {
    Hdr h; uint32_t sessionId; uint8_t slot, what, arg; uint8_t rsv; char name[16];
};
// IN_TEAM was added after kProtoVer 4 shipped, and deliberately does NOT bump it: PktIntent's
// {what, arg} layout is unchanged, so a peer that predates it simply ignores the intent
// instead of refusing the whole JOIN over a version mismatch.
enum : uint8_t { IN_READY = 1, IN_CHAR = 2, IN_COSTUME = 3, IN_NAME = 4, IN_LEAVE = 5,
                 IN_TEAM = 6 };

struct PktStart { Hdr h; uint32_t sessionId; LanMatchConfig cfg; uint32_t crc; };
struct PktReady { Hdr h; uint32_t sessionId, matchId, crc; uint8_t slot, rsv[3]; };
struct PktGo    { Hdr h; uint32_t sessionId, matchId; };
struct PktInput {
    Hdr h; uint32_t seed; uint32_t baseFrame;
    uint32_t need;              // the frame the SENDER is still waiting for (a rolling NACK)
    uint8_t slot, count, rsv[2];
    NetInput in[16];
};
struct PktPing  { Hdr h; uint32_t stamp; uint8_t slot, rsv[3]; };
struct PktBye   { Hdr h; uint32_t sessionId; uint8_t slot, reason, rsv[2]; };
#pragma pack(pop)

// ---------------------------------------------------------------- state
static SOCKET   g_sock = INVALID_SOCKET;
static int      g_port = 0;
#ifdef _WIN32
static bool     g_wsaUp = false;
#endif

static bool     g_armed = false;             // lockstep running
static bool     g_isHost = false;
// SEATS THIS PC DRIVES. Two people sharing one console is how this game was played, so one
// peer can own more than one seat: g_localSlot stays the PRIMARY seat (the one this PC's
// player 1 sits in, and the one lobby intents are addressed from), while g_localMask is every
// seat driven by a controller plugged into THIS machine. Local player j -- meaning pad j --
// owns the j-th set bit, in ascending seat order.
static int      g_localSlot = 0;
static uint8_t  g_localMask = 1;
static int      g_localCount = 1;
static inline bool IsLocalSlot(int s) {
    return s >= 0 && s < kSlots && (g_localMask & (1u << s)) != 0;
}
// Seat -> which local player drives it (0 = this PC's player 1), or -1 if it is not ours.
static int LocalIndexOf(int slot) {
    if (!IsLocalSlot(slot)) return -1;
    int j = 0;
    for (int s = 0; s < slot; ++s) if (IsLocalSlot(s)) ++j;
    return j;
}
// The n-th seat this PC drives, or -1.
static int LocalSlotAt(int j) {
    for (int s = 0; s < kSlots; ++s) if (IsLocalSlot(s) && j-- == 0) return s;
    return -1;
}
static int      g_slotCount = 2;
static int      g_delay = 2;
static int      g_delayForced = 0;           // TJ_NET_DELAY overrides the auto value
static bool     g_needInput[kSlots] = { true, true, false, false };

static NetInput g_ring[kSlots][kRing];
static uint32_t g_ringFrame[kSlots][kRing];
static bool     g_ringHave[kSlots][kRing];
static NetInput g_cur[kSlots];
// The frame each peer told us it is still waiting for. Without this the send window is
// anchored on the LOCAL frame, and a peer that falls more than D frames behind (one burst of
// loss is enough) can never be caught up: the peer ahead only ever resends [F, F+D], which
// no longer contains the frame the other one is stuck on. Measured under TJ_NET_DROP=8
// TJ_NET_BURST=5: host frozen at frame 3, joiner at frame 6, forever.
static uint32_t g_peerNeed[kSlots];
static const uint32_t kNoNeed = 0xFFFFFFFFu;

// legacy direct-peer mode (TJ_NET=host|join:<ip>): one hard-wired peer, armed at boot
static sockaddr_in g_legacyPeer{};
static bool     g_legacyMode = false, g_haveLegacyPeer = false;

// session
static LanState g_state = LAN_OFF;
static uint32_t g_sessionId = 0;
static uint32_t g_lobbyVer = 0;
static char     g_myName[16] = "PLAYER";
static char     g_gameName[16] = "";
static uint32_t g_pwHash = 0;                // host: required hash (0 = open)
static uint32_t g_joinPwHash = 0;            // client: hash we are offering
static LanNak   g_lastNak = NAK_NONE;
static char     g_status[96] = "";
static uint32_t g_buildHash = 0, g_dataHash = 0;
// ADVISORY ONLY — carried, logged and shown, never used to refuse anyone. moduleHash is the
// old buildHash (this module's own bytes); patchFp is xdk_patch's automatic fingerprint of
// every installed patch site.
static uint32_t g_moduleHash = 0, g_patchFp = 0;

struct Peer {
    sockaddr_in addr; uint32_t lastRecv, rtt; uint8_t slot; bool active; char name[16];
};
static Peer     g_peers[kSlots];             // index == slot (host owns its own entry)
static LanSlotInfo g_slots[kSlots];
static uint8_t  g_slotKind[kSlots];          // 0 open, 1 human, 2 cpu

static LanGameInfo g_games[kMaxGames];
static int      g_gameN = 0;

static LanMatchConfig g_cfg{};
static uint32_t g_cfgCrc = 0;
static bool     g_haveCfg = false;
static bool     g_readySeen[kSlots];
static bool     g_launchPending = false;
static uint32_t g_matchId = 0;

// host-side lobby rules (mirrored into the config at START)
static uint32_t g_ruleTime = 0xFFFFFFFFu;
static uint8_t  g_ruleMeat = 0;
static uint8_t  g_ruleRounds = 1, g_ruleDiff = 1, g_ruleMode = 0, g_ruleArena = 0,
                g_ruleFirstTo = 3;

// timers
static uint32_t g_tBeacon = 0, g_tPing = 0, g_tRetry = 0, g_tJoin = 0, g_tProbe = 0;

// diagnostics
static uint32_t g_stallFrames = 0, g_sent = 0, g_recv = 0, g_lastRemote = 0, g_dropped = 0;
static uint32_t g_rttMs = 0;

// `which` is the local player index on THIS PC (0 = first controller), not the seat.
extern "C" void GetLocalPadForNet(NetInput* out, int which);   // d3d8_bridge.cpp
// How many controllers are claimed on this PC right now -- i.e. how many people are sitting
// here. A pad claims a player number on its first press, so a second player must press
// something BEFORE hosting or joining to be counted.
extern "C" int  GetLocalPlayerCount();                          // d3d8_bridge.cpp

// ---------------------------------------------------------------- small helpers
static uint32_t Fnv32(const void* p, size_t n, uint32_t h = 2166136261u) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static uint32_t Crc32(const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p; uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= b[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}
static uint32_t Now() { return GetTickCount(); }

// Names are drawn through the game's own printf-based text path, so a '%' in a name is a
// live crash and a byte < 0x20 is a colour/icon escape. Sanitize at STORE time (on send
// AND on receive -- never trust the wire) so every consumer is safe by construction.
static void SanitizeName(char* dst, int cap, const char* src) {
    int o = 0;
    for (int i = 0; src && src[i] && o < cap - 1; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == ' ' || c == '-' || c == '\'' || c == '.';
        if (ok) dst[o++] = (char)c;
    }
    while (o > 0 && dst[o - 1] == ' ') --o;      // trailing spaces read as a blank name
    dst[o] = 0;
    if (!dst[0]) strncpy_s(dst, cap, "PLAYER", _TRUNCATE);
}

// Arabic when the player has chosen it, the English literal otherwise. Only the lines a
// player actually reads in normal play are translated; the Winsock/diagnostic ones stay
// English on purpose -- they name error codes and exist to be searched for.
static const char* L(uint16_t id, const char* en) {
    if (ArabicEnabled())
        if (const char* a = ArabicText(id)) return a;
    return en;
}
static void Status(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(g_status, sizeof(g_status), _TRUNCATE, fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------- build / data hashes
// A LAN match is "same original bytes, same inputs". Version skew, a different env-flag
// set or a partially applied patch is an immediate hard desync, so both are checked at
// JOIN and a mismatch is refused with a named reason instead of starting a doomed match.
// CONTENT, never timestamps: the same build copied to a second PC keeps its bytes but does
// not reliably keep its last-write time, and a spurious mismatch would refuse a perfectly
// good match.
static uint32_t HashFileContent(const char* path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    uint32_t h = 2166136261u;
    uint8_t buf[16384]; DWORD got = 0;
    while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got) h = Fnv32(buf, got, h);
    CloseHandle(f);
    return h;
}
// XOR-COMBINED, never order-dependent: FindFirstFile does not promise the same order on two
// machines (or two filesystems), and an order-sensitive hash would refuse the match for a
// reason nobody could diagnose.
static void ScanTree(const char* dir, uint32_t& acc, int depth, int& files) {
    if (depth > 4 || files > 20000) return;
    char pat[MAX_PATH]; _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char sub[MAX_PATH];
        _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ScanTree(sub, acc, depth + 1, files); continue; }
        ++files;
        char up[MAX_PATH];
        _snprintf_s(up, sizeof(up), _TRUNCATE, "%s", fd.cFileName);
        for (char* p = up; *p; ++p) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        uint32_t one = Fnv32(up, strlen(up));
        one = Fnv32(&fd.nFileSizeLow, 4, one);
        acc ^= one;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}
const char* AssetRoot();      // file_io.cpp
static void ComputeHashes() {
    if (g_buildHash) return;
    // ⚠ buildHash IS A SIM-CONTRACT HASH, NOT A BINARY HASH — and that distinction is what
    // makes PC<->phone play possible at all.
    //
    // It used to be `HashFileContent(our own module)`. That is the strongest possible proxy
    // for "both peers run identical sim logic" — automatic, impossible to forget — and it is
    // exactly right between two Windows PCs. But a Windows tj_hybrid.dll and an Android
    // libtjgame.so are different binaries BY CONSTRUCTION, so it refused every cross-platform
    // pair as "DIFFERENT VERSION" no matter how identical the simulation was. And it IS
    // identical: the determinism log is byte-for-byte the same on the physical phone over
    // 75,601 frames (gate S5c), and an interpreted joiner completed a full lockstep match
    // against a native host identical over 36,001 frames (gate S3e). The binaries differ; the
    // simulation does not.
    //
    // So the contract is now hashed from the things that ARE the simulation, all of which are
    // platform-independent:
    //   * kSimContract   — a version for changes a machine cannot see (a hook BODY changing
    //                      behaviour at the same site). ⚠ THE ONE MANUAL PIECE: bump it on any
    //                      sim-affecting change. This is the risk this design accepts.
    //   * kProtoVer      — wire compatibility.
    //   * the sim env flags — different inputs, different sim.
    // dataHash (below) already covers the game content, and is XOR-combined and case-folded
    // precisely so two filesystems agree.
    //
    // ⚠ WHY THE PATCH-SET FINGERPRINT IS *NOT* IN THE CONTRACT, though it is tempting and was
    // in the first version of this: the two platforms legitimately install DIFFERENT patch
    // sets. The sound backend differs (dsound shims vs the software mixer), the display path
    // differs, and the entry binary differs — so a fingerprint over every patch SITE encodes
    // host plumbing, not simulation, and refuses a cross-platform pair for reasons that have
    // nothing to do with whether the two sims agree. Measured: PC 114 sites, and the ARM build
    // does not install the same set. It is also ORDER- and TIMING-dependent (ComputeHashes can
    // run before every installer has), which a contract must never be.
    //
    // It still travels as patchFp — ADVISORY, exactly like moduleHash — so a genuine hook-set
    // difference between two peers of the SAME platform stays visible and diagnosable. Making
    // it authoritative would require classifying every site as sim-affecting or not, and
    // getting that classification wrong is a worse failure than the manual constant below.
    char dll[MAX_PATH] = {};
#if defined(_WIN32)
    // A code address is not an HMODULE: ask for the module CONTAINING this function.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ComputeHashes, &self);
    GetModuleFileNameA(self, dll, MAX_PATH);
#else
    GetModuleFileNameA(nullptr, dll, MAX_PATH);   // host_compat: this process's own image
#endif
    g_moduleHash = HashFileContent(dll);
    g_patchFp = tj::hybrid::PatchSetFingerprint();

    // ⚠ BUMP ON ANY SIM-AFFECTING CHANGE. 10: the contract dropped patchFp (see above), so
    // its VALUE changed even though nothing about the simulation did — and two peers must not
    // disagree silently about what a contract number means.
    static const uint32_t kSimContract = 10;
    uint32_t h = Fnv32(&kSimContract, 4);
    h = Fnv32(&kProtoVer, 1, h);
    const char* simEnv[] = { "TJ_UNLOCK", "TJ_NOINPUT" };
    for (const char* e : simEnv) {
        char* v = nullptr; size_t n = 0; _dupenv_s(&v, &n, e);
        uint8_t on = (v && *v && atoi(v)) ? 1 : 0; free(v);
        h = Fnv32(&on, 1, h);
    }
    g_buildHash = h ? h : 1;

    // dataHash covers the GAME DATA ONLY -- `default.xbe` by content, plus the three asset
    // trees by (name, size). It deliberately does NOT scan the whole asset root: in a shipped
    // folder the root also holds tj_log.txt, tomjerry.ini, screenshots and `_SAVES`, and
    // hashing those would make two PCs disagree the moment one of them saved a game or wrote
    // a log -- a JOIN refusal with no diagnosable cause.
    uint32_t d = 0; int files = 0;
    const char* root = AssetRoot();
    if (root && *root) {
        char xbe[MAX_PATH];
        _snprintf_s(xbe, sizeof xbe, _TRUNCATE, "%s\\default.xbe", root);
        d = HashFileContent(xbe);
        static const char* kDataDirs[] = { "GFX", "AUDMUSIC", "AUDSoundFX" };
        for (const char* sub : kDataDirs) {
            char p[MAX_PATH];
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s", root, sub);
            ScanTree(p, d, 0, files);
        }
    }
    d = Fnv32(&files, 4, d ? d : 1u);                // count guards the XOR against pairing
    g_dataHash = d ? d : 1;
    printf("[lan] contract=%08X data=%08X (%d asset files)  [advisory module=%08X patch=%08X]\n",
           g_buildHash, g_dataHash, files, g_moduleHash, g_patchFp);
}

// ---------------------------------------------------------------- fault injection
// TJ_NET_DROP=<pct>  drop this % of outgoing datagrams
// TJ_NET_BURST=<n>   when one drops, drop n-1 more (models a real loss burst)
// TJ_NET_LAG=<ms>    hold outgoing datagrams this long (+/- 50% jitter)
static int g_fiDrop = -1, g_fiBurst = 1, g_fiLag = 0, g_fiRun = 0;
static uint32_t g_fiRand = 0x2545F491u;
struct Held { uint32_t due; sockaddr_in to; int len; uint8_t buf[256]; };
static Held g_held[64]; static int g_heldN = 0;

static void FaultInit() {
    if (g_fiDrop >= 0) return;
    char* e = nullptr; size_t n = 0;
    _dupenv_s(&e, &n, "TJ_NET_DROP"); g_fiDrop = e && *e ? atoi(e) : 0; free(e);
    e = nullptr; _dupenv_s(&e, &n, "TJ_NET_BURST"); g_fiBurst = e && *e ? atoi(e) : 1; free(e);
    if (g_fiBurst < 1) g_fiBurst = 1;
    e = nullptr; _dupenv_s(&e, &n, "TJ_NET_LAG"); g_fiLag = e && *e ? atoi(e) : 0; free(e);
    if (g_fiDrop || g_fiLag)
        printf("[lan] FAULT INJECTION: drop=%d%% burst=%d lag=%dms\n", g_fiDrop, g_fiBurst, g_fiLag);
}
static uint32_t FiRand() { g_fiRand ^= g_fiRand << 13; g_fiRand ^= g_fiRand >> 17; g_fiRand ^= g_fiRand << 5; return g_fiRand; }

static void RawSend(const void* p, int len, const sockaddr_in& to) {
    sendto(g_sock, (const char*)p, len, 0, (const sockaddr*)&to, sizeof to);
    ++g_sent;
}
static void Send(const void* p, int len, const sockaddr_in& to) {
    if (g_sock == INVALID_SOCKET) return;
    FaultInit();
    if (g_fiDrop) {
        if (g_fiRun > 0) { --g_fiRun; ++g_dropped; return; }
        if ((int)(FiRand() % 100) < g_fiDrop) { g_fiRun = g_fiBurst - 1; ++g_dropped; return; }
    }
    if (g_fiLag && len <= (int)sizeof(g_held[0].buf) && g_heldN < 64) {
        Held& h = g_held[g_heldN++];
        uint32_t jitter = g_fiLag / 2;
        h.due = Now() + (uint32_t)g_fiLag - jitter + (FiRand() % (jitter * 2 + 1));
        h.to = to; h.len = len; memcpy(h.buf, p, len);
        return;
    }
    RawSend(p, len, to);
}
static void FlushHeld() {
    if (!g_heldN) return;
    uint32_t now = Now();
    for (int i = 0; i < g_heldN;) {
        if ((int32_t)(now - g_held[i].due) >= 0) {
            RawSend(g_held[i].buf, g_held[i].len, g_held[i].to);
            g_held[i] = g_held[--g_heldN];
        } else ++i;
    }
}

static void Hdrs(Hdr& h, uint8_t type) { h.magic = kMagic; h.ver = kProtoVer; h.type = type; h.pad = 0; }

// Every local IPv4 subnet's DIRECTED broadcast address (192.168.1.255 and friends), refreshed
// periodically because adapters come and go. The limited broadcast 255.255.255.255 is not
// enough on its own: on a multi-homed machine Windows sends it out ONE interface (whichever
// owns the default route), so a PC with Wi-Fi plus Ethernet, a VM switch or a VPN adapter can
// silently broadcast into the wrong network. Directed broadcasts are routed per subnet and
// reach every adapter.
static uint32_t g_bcast[8]; static int g_bcastN = 0; static uint32_t g_bcastAt = 0;
static void RefreshBroadcastList() {
    uint32_t now = Now();
    if (g_bcastN && now - g_bcastAt < 10000) return;
    g_bcastAt = now;
    g_bcastN = 0;
#ifdef _WIN32
    INTERFACE_INFO info[16]; DWORD got = 0;
    if (WSAIoctl(g_sock, SIO_GET_INTERFACE_LIST, nullptr, 0, info, sizeof info, &got,
                 nullptr, nullptr) == 0) {
        int n = (int)(got / sizeof(INTERFACE_INFO));
        for (int i = 0; i < n && g_bcastN < 8; ++i) {
            if (!(info[i].iiFlags & IFF_UP) || (info[i].iiFlags & IFF_LOOPBACK)) continue;
            uint32_t a = info[i].iiAddress.AddressIn.sin_addr.s_addr;
            uint32_t m = info[i].iiNetmask.AddressIn.sin_addr.s_addr;
            if (!a || !m) continue;
            g_bcast[g_bcastN++] = (a & m) | ~m;
        }
    }
#else
    // getifaddrs is the BSD spelling of SIO_GET_INTERFACE_LIST: same filter (up, not
    // loopback, has an IPv4 address + mask), same directed-broadcast arithmetic.
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* it = list; it && g_bcastN < 8; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET || !it->ifa_netmask)
                continue;
            if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
            uint32_t a = ((const sockaddr_in*)(const void*)it->ifa_addr)->sin_addr.s_addr;
            uint32_t m = ((const sockaddr_in*)(const void*)it->ifa_netmask)->sin_addr.s_addr;
            if (!a || !m) continue;
            g_bcast[g_bcastN++] = (a & m) | ~m;
        }
        freeifaddrs(list);
    }
#endif
    static int announced = 0;
    if (!announced++) {
        printf("[lan] broadcasting on %d subnet(s):", g_bcastN);
        for (int i = 0; i < g_bcastN; ++i) {
            in_addr t; t.s_addr = g_bcast[i];
            printf(" %s", inet_ntoa(t));
        }
        printf(" (+255.255.255.255, +127.0.0.1)\n");
    }
}
static void Broadcast(const void* p, int len) {
    RefreshBroadcastList();
    sockaddr_in to{}; to.sin_family = AF_INET;
    // NOTE: our OWN port is included. Two PCs both take 27100, so skipping it -- which is
    // fine when two instances share one machine and land on 27100/27101 -- meant the host
    // never broadcast to the port the other PC was listening on, and nothing was ever
    // discoverable across a real network. Hearing our own beacon is harmless: OnBeacon drops
    // any beacon carrying our own sessionId.
    for (int i = 0; i < kPortSpan; ++i) {
        to.sin_port = htons((u_short)(kBasePort + i));
        for (int b = 0; b < g_bcastN; ++b) {
            to.sin_addr.s_addr = g_bcast[b];
            Send(p, len, to);
        }
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        Send(p, len, to);
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);       // two windows on one PC
        Send(p, len, to);
    }
}
// One datagram per REMOTE MACHINE, not per remote seat. Two players sharing a PC occupy two
// seats behind one address, and sending to both would duplicate every lobby update and every
// input window at them -- harmless for correctness, but it doubles their traffic and makes the
// packet counters lie.
static void SendToPeersExcept(const void* p, int len, const sockaddr_in* except) {
    sockaddr_in sent[kSlots];
    int n = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (!g_peers[i].active || IsLocalSlot(i)) continue;
        if (except && g_peers[i].addr.sin_addr.s_addr == except->sin_addr.s_addr &&
                      g_peers[i].addr.sin_port == except->sin_port) continue;
        bool dup = false;
        for (int k = 0; k < n && !dup; ++k)
            dup = sent[k].sin_addr.s_addr == g_peers[i].addr.sin_addr.s_addr &&
                  sent[k].sin_port == g_peers[i].addr.sin_port;
        if (dup) continue;
        sent[n++] = g_peers[i].addr;
        Send(p, len, g_peers[i].addr);
    }
}
static void SendToPeers(const void* p, int len) { SendToPeersExcept(p, len, nullptr); }

// ---------------------------------------------------------------- input ring
static void RingPut(int slot, uint32_t frame, const NetInput& in) {
    int i = (int)(frame & (kRing - 1));
    g_ring[slot][i] = in;
    g_ringFrame[slot][i] = frame;
    g_ringHave[slot][i] = true;
}
static bool RingGet(int slot, uint32_t frame, NetInput* out) {
    int i = (int)(frame & (kRing - 1));
    if (!g_ringHave[slot][i] || g_ringFrame[slot][i] != frame) return false;
    *out = g_ring[slot][i];
    return true;
}
static void RingReset() {
    memset(g_ringHave, 0, sizeof g_ringHave);
    memset(g_cur, 0, sizeof g_cur);
    for (int s = 0; s < kSlots; ++s) g_peerNeed[s] = kNoNeed;
    NetInput z{};
    for (uint32_t f = 0; f < (uint32_t)g_delay; ++f)
        for (int s = 0; s < kSlots; ++s) RingPut(s, f, z);
}

bool NetArmed() { return g_armed; }

// ---------------------------------------------------------------- socket
static bool OpenSocket() {
    if (g_sock != INVALID_SOCKET) return true;
#ifdef _WIN32
    if (!g_wsaUp) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Status("WINSOCK FAILED"); return false; }
        g_wsaUp = true;
    }
#endif
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { Status("SOCKET FAILED (%d)", WSAGetLastError()); return false; }
    u_long nb = 1; ioctlsocket(g_sock, FIONBIO, &nb);
    BOOL yes = TRUE;
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof yes);
#ifdef _WIN32
    // A UDP peer that receives an ICMP port-unreachable (nobody listening on one of the
    // swept broadcast ports) otherwise gets WSAECONNRESET on the NEXT recvfrom and looks
    // like a dead socket.
    DWORD off = 0; DWORD ret = 0;
    WSAIoctl(g_sock, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &off, sizeof off,
             nullptr, 0, &ret, nullptr, nullptr);
#endif
    // (No POSIX half: Linux only surfaces ICMP unreachables on CONNECTED sockets, and this
    // socket never connect()s -- the failure mode SIO_UDP_CONNRESET disables cannot occur.)

    for (int i = 0; i < kPortSpan; ++i) {
        sockaddr_in me{}; me.sin_family = AF_INET;
        me.sin_addr.s_addr = htonl(INADDR_ANY);
        me.sin_port = htons((u_short)(kBasePort + i));
        if (bind(g_sock, (sockaddr*)&me, sizeof me) == 0) {
            g_port = kBasePort + i;
            printf("[lan] socket bound to UDP %d\n", g_port);
            return true;
        }
    }
    int err = WSAGetLastError();
    printf("[lan] bind FAILED on %d-%d err=%d\n", kBasePort, kBasePort + kPortSpan - 1, err);
    Status("CANNOT OPEN UDP %d-%d (ERROR %d)", kBasePort, kBasePort + kPortSpan - 1, err);
    closesocket(g_sock); g_sock = INVALID_SOCKET;
    return false;
}

// The name a second (or third) person at the same PC appears under: "SHIHAB", "SHIHAB 2".
// One base name is typed per machine, so the extra seats are derived rather than asked for --
// and the suffix has to fit the 16-byte name field, so the base is trimmed, not the number.
static void LocalPlayerName(char* out, size_t cap, const char* base, int localIndex) {
    if (localIndex <= 0) { strncpy_s(out, cap, base, _TRUNCATE); return; }
    char trimmed[16];
    strncpy_s(trimmed, base, _TRUNCATE);
    size_t room = cap - 3;                       // " N" + NUL
    if (strlen(trimmed) > room) trimmed[room] = 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s %d", trimmed, localIndex + 1);
}

// ---------------------------------------------------------------- lobby helpers
static void LobbyReset(bool host) {
    memset(g_peers, 0, sizeof g_peers);
    memset(g_slots, 0, sizeof g_slots);
    memset(g_slotKind, 0, sizeof g_slotKind);
    memset(g_readySeen, 0, sizeof g_readySeen);
    g_lobbyVer = 0;
    if (host) {
        // The host takes one seat per person sitting at it, starting at 0.
        int n = GetLocalPlayerCount();
        if (n < 1) n = 1; if (n > kSlots) n = kSlots;
        g_localSlot = 0;
        g_localCount = n;
        g_localMask = (uint8_t)((1u << n) - 1u);
        for (int i = 0; i < kSlots; ++i) { g_slots[i].charId = (uint8_t)i; g_slots[i].team = (uint8_t)i; }
        for (int i = 0; i < n; ++i) {
            g_peers[i].active = true; g_peers[i].slot = (uint8_t)i; g_peers[i].lastRecv = Now();
            LocalPlayerName(g_peers[i].name, 16, g_myName, i);
            g_slotKind[i] = 1;
            strncpy_s(g_slots[i].name, g_peers[i].name, _TRUNCATE);
            g_slots[i].kind = 1; g_slots[i].isLocal = 1;
        }
        if (n > 1) printf("[lan] hosting with %d players on this PC\n", n);
    }
}
static void PublishSlots() {
    for (int i = 0; i < kSlots; ++i) {
        g_slots[i].kind = g_slotKind[i];
        g_slots[i].isLocal = IsLocalSlot(i) ? 1 : 0;
        if (g_slotKind[i] && !g_slots[i].name[0])
            _snprintf_s(g_slots[i].name, sizeof(g_slots[i].name), _TRUNCATE, "P%d", i + 1);
    }
}
static int HostPlayerCount() {
    int n = 0; for (int i = 0; i < kSlots; ++i) if (g_slotKind[i] == 1) ++n; return n;
}

static void SendLobbyState() {
    PktLobby p{}; Hdrs(p.h, P_LOBBY);
    p.sessionId = g_sessionId; p.lobbyVer = ++g_lobbyVer;
    p.players = (uint8_t)kSlots; p.mode = g_ruleMode; p.arena = g_ruleArena;
    for (int i = 0; i < kSlots; ++i) {
        p.kind[i] = g_slotKind[i]; p.charId[i] = g_slots[i].charId;
        p.costume[i] = g_slots[i].costume; p.team[i] = g_slots[i].team;
        p.ready[i] = g_slots[i].ready;
        memcpy(p.name[i], g_slots[i].name, 16);
    }
    p.timeLimit = g_ruleTime; p.rounds = g_ruleRounds; p.difficulty = g_ruleDiff;
    p.meat = g_ruleMeat;
    p.tournFirstTo = g_ruleFirstTo; p.inputDelay = (uint8_t)g_delay;
    SendToPeers(&p, sizeof p);
}

// The lockstep barrier needs a per-slot "does this slot receive network input" map:
// CPU and empty slots are driven by the (identically seeded) AI, so they must NOT be
// waited on -- and their pad must read as a deterministic neutral, never local hardware.
// Has each peer sent anything since we armed? Its first INPUT packet is the only proof it
// ever received GO -- there is no acknowledgement for GO itself.
static bool g_peerInputSeen[kSlots];
static uint32_t g_tGo = 0;

static void ArmLockstep(const LanMatchConfig& c) {
    g_slotCount = kSlots;
    for (int i = 0; i < kSlots; ++i) { g_needInput[i] = (c.slotKind[i] == 1); g_peerInputSeen[i] = false; }
    g_delay = c.inputDelay ? c.inputDelay : 2;
    NetSyncSetSeed(c.seed);
    NetSyncResetFrame();
    RingReset();
    g_armed = true;
    g_state = LAN_MATCH;
    g_launchPending = true;
    printf("[lan] ARMED match %08X seed=%u delay=%d slots=%d%d%d%d\n", c.matchId, c.seed,
           g_delay, c.slotKind[0], c.slotKind[1], c.slotKind[2], c.slotKind[3]);
}

static void Disarm(const char* why) {
    if (!g_armed) return;
    g_armed = false;
    g_launchPending = false;
    NetSyncLogMark("disarm");     // det_diff stops comparing here (the lobby is not lockstepped)
    printf("[lan] disarmed (%s)\n", why);
}

// ---------------------------------------------------------------- config
uint32_t LanConfigCrc(const LanMatchConfig* c) { return Crc32(c, sizeof(LanMatchConfig)); }
const LanMatchConfig* LanConfig() { return g_haveCfg ? &g_cfg : nullptr; }

// CANONICAL FORM. Every peer clamps the config to the same legal values before hashing it,
// so the CRCs exchanged in START/BARRIER_READY compare the bytes the simulation will
// actually run -- not the bytes that happened to be on the wire. Two consequences, both
// wanted: a receiver that has to change anything necessarily disagrees with the sender's
// CRC and the start is refused loudly, and no out-of-range value ever reaches the game
// (an illegal settings byte makes the retail validator reset the blob and wipe unlocks).
static void CanonConfig(LanMatchConfig& c) {
    if (c.timeLimit != 0xFFFFFFFFu && c.timeLimit != 0xE10u &&
        c.timeLimit != 0x1518u && c.timeLimit != 0x1C20u) c.timeLimit = 0x1518u;
    if (c.rounds != 1 && c.rounds != 3 && c.rounds != 5) c.rounds = 1;
    if (c.difficulty > 2) c.difficulty = 1;
    // CanonConfig IS the CRC contract: an unclamped field makes two peers hash different
    // bytes and the START is refused. Only the six legal MEAT RUSH values survive.
    if (c.meat != 0 && c.meat != 5 && c.meat != 10 && c.meat != 15 &&
        c.meat != 20 && c.meat != 0xFF) c.meat = 0;
    if (c.arena >= 13) c.arena = 0;
    if (c.mode > 2) c.mode = 0;          // 0 QUICK MATCH, 1 TOURNAMENT, 2 MEAT RUSH
    if (c.teamMode > 2) c.teamMode = 0;
    if (c.inputDelay < 1 || c.inputDelay > 8) c.inputDelay = 2;
    if (!c.tournFirstTo || c.tournFirstTo > 9) c.tournFirstTo = 3;
    c.players = kSlots;
    for (int i = 0; i < kSlots; ++i) {
        if (c.slotKind[i] > 2) c.slotKind[i] = 0;
        if (c.slotChar[i] >= 11) c.slotChar[i] = 0;
        // Against the character's own ceiling, not a flat 6 -- five of the eleven have
        // exactly one costume, and the canonical config is what both peers hash.
        if (c.slotCostume[i] >= LanCostumeCount(c.slotChar[i])) c.slotCostume[i] = 0;
        if (c.slotTeam[i] >= 4) c.slotTeam[i] = (uint8_t)i;
        char tmp[16] = "";
        if (c.slotKind[i] == 1) SanitizeName(tmp, 16, c.slotName[i]);
        memset(c.slotName[i], 0, 16);
        memcpy(c.slotName[i], tmp, strlen(tmp));
    }
}

static void BuildConfig(LanMatchConfig& c) {
    memset(&c, 0, sizeof c);
    c.matchId = ++g_matchId ? g_matchId : ++g_matchId;
    // The seed is minted by the HOST and is what makes item spawns, hazards and AI
    // identical on every peer (both RNGs are reseeded from it at the mode-6 barrier).
    uint32_t s = NetSyncGetSeed() * 1664525u + 1013904223u + Now();
    c.seed = s ? s : 1;
    c.timeLimit = g_ruleTime;
    c.players = (uint8_t)kSlots;
    c.inputDelay = (uint8_t)g_delay;
    c.arena = g_ruleArena; c.mode = g_ruleMode;
    // THE TEAM LAYOUT IS DERIVED, NOT CHOSEN -- that is what retail does. FUN_000247A0 sets
    // screen11+0xA0 to 1 (TEAM) the moment two seats share a letter and back to 0 (MELEE)
    // when they do not, so the lobby offers letters and the layout follows. Deriving it also
    // keeps it off the wire: every peer already has team[4] from PktLobby and computes the
    // same answer, so PktLobby's layout is untouched and kProtoVer stays 4.
    // NOTE the MELEE branch of the retail commit (0x25FC0) OVERWRITES all four letters with
    // 0,1,2,3 -- so sending slotTeam without a matching teamMode achieves exactly nothing.
    {
        uint8_t n[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < kSlots; ++i) if (g_slotKind[i]) ++n[g_slots[i].team & 3];
        c.teamMode = (n[0] > 1 || n[1] > 1 || n[2] > 1 || n[3] > 1) ? 1 : 0;
    }
    c.rounds = g_ruleRounds; c.difficulty = g_ruleDiff; c.tournFirstTo = g_ruleFirstTo;
    c.meat = g_ruleMeat;
    for (int i = 0; i < kSlots; ++i) {
        c.slotKind[i] = g_slotKind[i];
        c.slotChar[i] = g_slots[i].charId;
        c.slotCostume[i] = g_slots[i].costume;
        c.slotTeam[i] = g_slots[i].team;
        memcpy(c.slotName[i], g_slots[i].name, 16);
    }
    CanonConfig(c);
}

// ---------------------------------------------------------------- receive
static void SendJoinReq();
static void FillBeacon(PktBeacon& b) {
    Hdrs(b.h, P_BEACON);
    b.sessionId = g_sessionId; b.buildHash = g_buildHash; b.dataHash = g_dataHash;
    b.moduleHash = g_moduleHash; b.patchFp = g_patchFp;
    memcpy(b.host, g_gameName, 16);
    b.players = (uint8_t)HostPlayerCount(); b.maxPlayers = kSlots;
    b.mode = g_ruleMode; b.arena = g_ruleArena;
    b.locked = g_pwHash ? 1 : 0;                  // the password itself NEVER rides here
    b.inMatch = (g_state == LAN_MATCH || g_state == LAN_STARTING) ? 1 : 0;
}
// A PROBE is answered with a UNICAST beacon straight back to the asker. Broadcast can be
// blocked in one direction (a firewall profile, AP client isolation, a VPN adapter eating the
// route) while ordinary unicast still works perfectly -- and the probe already proved the
// client can reach us, so the reply path is known good.
static void OnProbe(const sockaddr_in& from) {
    PktBeacon b{}; FillBeacon(b);
    Send(&b, sizeof b, from);
}

static void OnBeacon(const PktBeacon& b, const sockaddr_in& from) {
    if (g_isHost && b.sessionId == g_sessionId) return;
    int slot = -1;
    for (int i = 0; i < g_gameN; ++i)
        if (g_games[i].sessionId == b.sessionId) { slot = i; break; }
    if (slot < 0) {
        if (g_gameN >= kMaxGames) return;
        slot = g_gameN++;
        memset(&g_games[slot], 0, sizeof g_games[slot]);
    }
    LanGameInfo& g = g_games[slot];
    g.sessionId = b.sessionId;
    g.ip = from.sin_addr.s_addr; g.port = from.sin_port;
    memcpy(g.host, b.host, 16); g.host[15] = 0;
    g.players = b.players; g.maxPlayers = b.maxPlayers; g.mode = b.mode; g.arena = b.arena;
    g.locked = b.locked; g.inMatch = b.inMatch;
    g.compatible = (b.buildHash == g_buildHash && b.dataHash == g_dataHash) ? 1 : 0;
    g.sameBuild  = (b.moduleHash == g_moduleHash) ? 1 : 0;
    // A joinable peer running a DIFFERENT BINARY is the normal cross-platform case, not a
    // fault — but it is worth saying out loud exactly once per session, because if a desync
    // ever does turn up this line is the first thing anyone will want to see.
    if (g.compatible && !g.sameBuild && !g.notedBuild) {
        g.notedBuild = 1;
        printf("[lan] '%s' runs a different BUILD (module %08X vs %08X, patch %08X vs %08X) "
               "but the same SIM CONTRACT %08X — joining allowed\n",
               g.host, b.moduleHash, g_moduleHash, b.patchFp, g_patchFp, g_buildHash);
    }
    g.ageMs = Now();
    // Joining by address is aimed at a HOST, not at a port -- the other PC took the first
    // free port in the range, which is usually but not always 27100. Its unicast beacon
    // reply tells us exactly where it is, so adopt that and re-send the request there.
    if (!g_isHost && g_state == LAN_JOINING && g_sessionId == 0 &&
        from.sin_addr.s_addr == g_peers[0].addr.sin_addr.s_addr) {
        g_sessionId = b.sessionId;
        g_peers[0].addr = from;
        printf("[lan] host answered from %s:%d\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
        SendJoinReq();
    }
}

static void OnJoinReq(const PktJoinReq& q, const sockaddr_in& from, int len) {
    if (!g_isHost) return;
    PktJoinRsp r{}; Hdrs(r.h, P_JOIN_RSP);
    r.sessionId = g_sessionId; r.buildHash = g_buildHash; r.dataHash = g_dataHash;
    r.moduleHash = g_moduleHash; r.patchFp = g_patchFp;
    auto nak = [&](LanNak n) { r.ok = 0; r.nak = (uint8_t)n; Send(&r, sizeof r, from); };
    if (q.h.ver != kProtoVer)          { nak(NAK_PROTO); return; }
    if (q.buildHash != g_buildHash)    { nak(NAK_BUILD); return; }
    if (q.dataHash != g_dataHash)      { nak(NAK_DATA); return; }
    if (g_pwHash && q.pwHash != g_pwHash) {
        printf("[lan] JOIN refused: wrong password\n");
        nak(NAK_PASSWORD); return;
    }
    if (g_state == LAN_MATCH || g_state == LAN_STARTING) { nak(NAK_INMATCH); return; }
    // How many people are at that PC? Clamped hard: a malformed or hostile count must not be
    // able to claim the whole lobby.
    int want = (len >= (int)sizeof(PktJoinReq) && q.players) ? q.players : 1;
    if (want < 1) want = 1;
    if (want > kSlots - 1) want = kSlots - 1;

    // Already known? (JOIN_REQ is retried until answered, so this must be idempotent -- and
    // for a 2-player PC that means returning the SAME seats, not allocating another pair.)
    uint8_t mask = 0;
    int slot = -1, have = 0;
    for (int i = 1; i < kSlots; ++i)
        if (g_peers[i].active && g_peers[i].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
            g_peers[i].addr.sin_port == from.sin_port) {
            if (slot < 0) slot = i;
            mask |= (uint8_t)(1u << i);
            ++have;
        }
    // Top up rather than only allocating on a first join: someone can plug a second controller
    // in AFTER joining, and the re-sent JOIN_REQ then asks for more seats than they hold. Seats
    // are never taken away here -- a pad being unplugged mid-lobby leaves the seat sitting
    // there, which is far better than silently evicting a player who nudged a cable.
    for (int i = 1; i < kSlots && have < want; ++i) {
        if (g_peers[i].active) continue;
        if (slot < 0) slot = i;
        mask |= (uint8_t)(1u << i);
        ++have;
    }
    if (slot < 0) { nak(NAK_FULL); return; }

    int localIdx = 0;
    for (int i = 1; i < kSlots; ++i) {
        if (!(mask & (1u << i))) continue;
        g_peers[i].active = true; g_peers[i].addr = from; g_peers[i].slot = (uint8_t)i;
        g_peers[i].lastRecv = Now();
        char base[16];
        SanitizeName(base, 16, q.name);
        LocalPlayerName(g_peers[i].name, 16, base, localIdx);
        g_slotKind[i] = 1;
        memcpy(g_slots[i].name, g_peers[i].name, 16);
        g_slots[i].ready = 0;
        ++localIdx;
    }
    PublishSlots();
    r.ok = 1; r.slot = (uint8_t)slot; r.slotMask = mask;
    r.players = (uint8_t)HostPlayerCount();
    Send(&r, sizeof r, from);
    printf("[lan] %s joined as slot %d (mask %X, %d player(s)) (%s:%d)\n", g_peers[slot].name,
           slot, mask, localIdx, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
    Status("%s %s", L(0x270, "JOINED"), g_peers[slot].name);
    SendLobbyState();
}

static void OnJoinRsp(const PktJoinRsp& r, const sockaddr_in& from, int len) {
    // A JOIN_RSP also arrives in the LOBBY: that is the host answering a re-sent JOIN_REQ from
    // someone who has just plugged in another controller, and it carries their new seat mask.
    if (g_state == LAN_LOBBY && !g_isHost && r.ok && r.sessionId == g_sessionId) {
        uint8_t mask = (len >= (int)sizeof(PktJoinRsp) && r.slotMask)
                     ? r.slotMask : (uint8_t)(1u << r.slot);
        if (mask != g_localMask) {
            g_localMask = mask;
            g_localCount = 0;
            for (int i = 0; i < kSlots; ++i) if (g_localMask & (1u << i)) ++g_localCount;
            printf("[lan] seats updated: this PC now drives %d (mask %X)\n", g_localCount, mask);
            LanSetName(g_myName);        // re-derive "<name> 2" for the seat we just gained
        }
        return;
    }
    if (g_state != LAN_JOINING) return;
    if (!r.ok) {
        g_lastNak = (LanNak)r.nak;
        g_state = LAN_BROWSE;
        printf("[lan] JOIN refused: %s\n", LanNakText());
        Status("%s", LanNakText());
        return;
    }
    g_lastNak = NAK_NONE;
    g_sessionId = r.sessionId;
    g_localSlot = r.slot;
    // The host decides how many seats we actually got -- it may have had fewer free than we
    // asked for, so trust the granted mask over our own request.
    g_localMask = (len >= (int)sizeof(PktJoinRsp) && r.slotMask)
                ? r.slotMask : (uint8_t)(1u << r.slot);
    g_localCount = 0;
    for (int i = 0; i < kSlots; ++i) if (g_localMask & (1u << i)) ++g_localCount;
    if (g_localCount > 1) printf("[lan] this PC drives %d seats (mask %X)\n", g_localCount, g_localMask);
    g_isHost = false;
    memset(g_peers, 0, sizeof g_peers);
    g_peers[0].active = true; g_peers[0].addr = from; g_peers[0].lastRecv = Now();
    g_state = LAN_LOBBY;
    printf("[lan] joined session %08X as slot %d\n", r.sessionId, r.slot);
    Status("%s %d", L(0x272, "JOINED - SLOT"), r.slot + 1);
}

static void OnLobby(const PktLobby& p, const sockaddr_in& from) {
    if (g_isHost || p.sessionId != g_sessionId) return;
    if (g_state == LAN_BROWSE || g_state == LAN_JOINING) g_state = LAN_LOBBY;
    g_peers[0].active = true; g_peers[0].addr = from; g_peers[0].lastRecv = Now();
    g_lobbyVer = p.lobbyVer;
    for (int i = 0; i < kSlots; ++i) {
        g_slotKind[i] = p.kind[i];
        g_slots[i].charId = p.charId[i]; g_slots[i].costume = p.costume[i];
        g_slots[i].team = p.team[i];     g_slots[i].ready = p.ready[i];
        SanitizeName(g_slots[i].name, 16, p.name[i]);
    }
    g_ruleMode = p.mode; g_ruleArena = p.arena; g_ruleTime = p.timeLimit;
    g_ruleRounds = p.rounds; g_ruleDiff = p.difficulty; g_ruleFirstTo = p.tournFirstTo;
    g_ruleMeat = p.meat;
    if (!g_delayForced && p.inputDelay) g_delay = p.inputDelay;
    PublishSlots();
}

static void OnIntent(const PktIntent& p) {
    if (!g_isHost || p.sessionId != g_sessionId || p.slot >= kSlots) return;
    LanSlotInfo& s = g_slots[p.slot];
    switch (p.what) {
    case IN_READY:   s.ready = p.arg ? 1 : 0; break;
    case IN_CHAR:    s.charId = p.arg;
                     if (s.costume >= LanCostumeCount(s.charId)) s.costume = 0;
                     break;
    case IN_COSTUME: s.costume = p.arg < LanCostumeCount(s.charId) ? p.arg : 0; break;
    case IN_TEAM:    s.team = p.arg & 3; break;
    case IN_NAME:    SanitizeName(s.name, 16, p.name); break;
    case IN_LEAVE:
        g_peers[p.slot].active = false; g_slotKind[p.slot] = 0;
        s.ready = 0; s.name[0] = 0;
        Status("%s %s", L(0x271, "LEFT"), p.name);
        break;
    }
    PublishSlots();
    SendLobbyState();
}

static void OnStart(const PktStart& p) {
    if (g_isHost || p.sessionId != g_sessionId) return;
    LanMatchConfig cfg = p.cfg;
    CanonConfig(cfg);
    uint32_t crc = LanConfigCrc(&cfg);
    if (crc != p.crc) { printf("[lan] START rejected: config CRC %08X != %08X\n", crc, p.crc); return; }
    if (g_state == LAN_MATCH && g_cfg.matchId == cfg.matchId) return;   // duplicate
    g_cfg = cfg; g_cfgCrc = crc; g_haveCfg = true;
    g_state = LAN_STARTING;
    Status("%s", L(0x256, "STARTING MATCH..."));
}

static void OnReady(const PktReady& p) {
    if (!g_isHost || p.sessionId != g_sessionId || p.slot >= kSlots) return;
    if (p.matchId != g_cfg.matchId) return;
    if (p.crc != g_cfgCrc) {
        // Every peer answers with ITS OWN CRC of the canonical config, so a mismatch names a
        // real disagreement about what is being started rather than starting a doomed match.
        printf("[lan] slot %d reports config CRC %08X, ours is %08X -- ABORTING START\n",
               p.slot, p.crc, g_cfgCrc);
        Status("%s", L(0x27B, "CONFIG MISMATCH - START ABORTED"));
        g_state = LAN_LOBBY;
        return;
    }
    g_readySeen[p.slot] = true;
}

static void OnGo(const PktGo& p) {
    if (g_isHost || p.sessionId != g_sessionId) return;
    if (!g_haveCfg || p.matchId != g_cfg.matchId) return;
    if (g_armed) return;
    ArmLockstep(g_cfg);
}

static void OnInput(const PktInput& p, const sockaddr_in& from) {
    if (p.slot >= (uint32_t)kSlots) return;
    // The host's seed is authoritative: a joiner adopts it so both peers seed their
    // generators identically at the match barrier. In session mode it also arrives in the
    // START config; the legacy TJ_NET harness relies on this path alone.
    if (!g_isHost && p.slot == 0 && p.seed) NetSyncSetSeed(p.seed);
    if (g_legacyMode && !g_haveLegacyPeer) {
        g_legacyPeer = from; g_haveLegacyPeer = true;
        printf("[net] peer connected from %s:%d\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
    }
    uint32_t n = p.count > 16 ? 16 : p.count;
    for (uint32_t i = 0; i < n; ++i) RingPut((int)p.slot, p.baseFrame + i, p.in[i]);
    if (n) g_lastRemote = p.baseFrame + n - 1;
    g_peerNeed[p.slot] = p.need;
    if (p.slot < (uint32_t)kSlots) { g_peers[p.slot].lastRecv = Now(); g_peerInputSeen[p.slot] = true; }

    // ⚠ THE HOST RELAYS. The session topology is a STAR: a joiner only ever learns the
    // HOST's address (OnJoinRsp/OnLobby set g_peers[0] and nothing else), so joiner B and
    // joiner C never exchange a single packet. With two machines that is invisible -- the
    // one peer each side knows IS the other player. With three it deadlocks: the lockstep
    // barrier in NetTickBegin needs an input for every human seat, B never receives C's, C
    // never receives B's, and because a stalled peer stops producing new input the host
    // starves too. Reproduced with three instances: all three sat at "stalled at frame 3,
    // waiting for peer" forever, frame counter pinned at 0. Players reported it as a crash.
    //
    // Forwarding the datagram VERBATIM also fixes the catch-up window for free: the packet
    // carries the sender's own `need`, so the store above gives every peer every other
    // peer's rolling NACK, exactly as a direct mesh would. Sent to every machine except the
    // one it came from, so nothing can loop.
    if (g_isHost && !g_legacyMode && p.slot < (uint32_t)kSlots && !IsLocalSlot((int)p.slot))
        SendToPeersExcept(&p, (int)sizeof p, &from);
}

static void PumpRecv() {
    if (g_sock == INVALID_SOCKET) return;
    uint8_t buf[1024];
    for (int guard = 0; guard < 512; ++guard) {
#ifdef _WIN32
        sockaddr_in from{}; int flen = sizeof from;
#else
        sockaddr_in from{}; socklen_t flen = sizeof from;
#endif
        int r = recvfrom(g_sock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &flen);
        if (r <= 0) break;
        if (r < (int)sizeof(Hdr)) continue;
        const Hdr* h = (const Hdr*)buf;
        if (h->magic != kMagic) continue;
        ++g_recv;
        // ANY packet from a known peer is proof of life. Refreshing only on JOIN and INPUT
        // timed clients out of an idle lobby after 5 s -- they had nothing to say, and their
        // 4 Hz PING was answered but never counted.
        //
        // ⚠ EVERY SEAT AT THAT ADDRESS, not just the first. Two people sharing a PC are two
        // peer entries behind ONE address, and their PING carries only the primary seat's
        // number -- so stopping at the first match left the second seat's clock frozen and the
        // host evicted that player a few seconds into the lobby, every time.
        for (int s = 0; s < kSlots; ++s)
            if (g_peers[s].active &&
                g_peers[s].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                g_peers[s].addr.sin_port == from.sin_port) g_peers[s].lastRecv = Now();
        switch (h->type) {
        case P_BEACON: if (r >= (int)sizeof(PktBeacon)) OnBeacon(*(PktBeacon*)buf, from); break;
        case P_PROBE:
            if (g_isHost && g_state != LAN_OFF) OnProbe(from);   // unicast reply, right now
            break;
        case P_JOIN_REQ: if (r >= kJoinReqV5) OnJoinReq(*(PktJoinReq*)buf, from, r); break;
        case P_JOIN_RSP: if (r >= kJoinRspV5) OnJoinRsp(*(PktJoinRsp*)buf, from, r); break;
        case P_LOBBY:    if (r >= (int)sizeof(PktLobby))   OnLobby(*(PktLobby*)buf, from); break;
        case P_INTENT:   if (r >= (int)sizeof(PktIntent))  OnIntent(*(PktIntent*)buf); break;
        case P_START:    if (r >= (int)sizeof(PktStart))   OnStart(*(PktStart*)buf); break;
        case P_READY:    if (r >= (int)sizeof(PktReady))   OnReady(*(PktReady*)buf); break;
        case P_GO:       if (r >= (int)sizeof(PktGo))      OnGo(*(PktGo*)buf); break;
        case P_INPUT:    if (r >= (int)sizeof(PktInput))   OnInput(*(PktInput*)buf, from); break;
        case P_PING: {
            if (r < (int)sizeof(PktPing)) break;
            PktPing q = *(PktPing*)buf; Hdrs(q.h, P_PONG); Send(&q, sizeof q, from);
            break;
        }
        case P_PONG: {
            if (r < (int)sizeof(PktPing)) break;
            const PktPing& q = *(PktPing*)buf;
            uint32_t rtt = Now() - q.stamp;
            g_rttMs = g_rttMs ? (g_rttMs * 3 + rtt) / 4 : rtt;
            if (q.slot < kSlots) g_peers[q.slot].rtt = rtt;
            for (int i = 0; i < g_gameN; ++i)
                if (g_games[i].ip == from.sin_addr.s_addr) g_games[i].rttMs = rtt;
            break;
        }
        case P_BYE: {
            if (r < (int)sizeof(PktBye)) break;
            const PktBye& b = *(PktBye*)buf;
            if (b.slot < kSlots) {
                printf("[lan] slot %d left (reason %d)\n", b.slot, b.reason);
                if (g_isHost) { g_peers[b.slot].active = false; g_slotKind[b.slot] = 0;
                                g_slots[b.slot].ready = 0; g_slots[b.slot].name[0] = 0;
                                PublishSlots(); SendLobbyState(); }
                else if (b.slot == 0) { Status("%s", L(0x273, "HOST LEFT THE GAME")); LanClose("host left"); }
                else if (IsLocalSlot(b.slot)) {
                    // The host removed one of us. If it was this PC's only seat we are out of
                    // the game; if two people were sharing the machine, the other one plays on.
                    g_localMask &= (uint8_t)~(1u << b.slot);
                    g_localCount = 0;
                    for (int i = 0; i < kSlots; ++i) if (g_localMask & (1u << i)) ++g_localCount;
                    if (!g_localCount) { Status("%s", L(0x274, "REMOVED BY THE HOST")); LanClose("kicked"); }
                    else {
                        g_localSlot = LocalSlotAt(0);
                        Status("%s", L(0x275, "A PLAYER WAS REMOVED"));
                        printf("[lan] host removed our seat %d; %d left\n", b.slot, g_localCount);
                    }
                }
            }
            break;
        }
        default: break;
        }
    }
}

// ---------------------------------------------------------------- legacy direct mode
// TJ_NET=host | join:<ip> -- the stage-1c harness. Arms at boot with a hard-wired peer and
// no session/lobby, which is what every determinism A/B run in PROJECT_STATUS.md uses.
// Kept byte-for-byte equivalent on the wire so those recipes stay valid.
bool NetInit() {
    char* e = nullptr; size_t n = 0;
    _dupenv_s(&e, &n, "TJ_NET");
    if (!e || !*e) { free(e); return false; }
    char mode[128]; strncpy_s(mode, e, _TRUNCATE); free(e);

    e = nullptr; _dupenv_s(&e, &n, "TJ_NET_DELAY");
    if (e && *e) { g_delay = atoi(e); if (g_delay < 1) g_delay = 1; if (g_delay > 16) g_delay = 16;
                   g_delayForced = 1; }
    free(e);
    e = nullptr; _dupenv_s(&e, &n, "TJ_NET_PLAYERS");
    int players = e && *e ? atoi(e) : 2; free(e);
    if (players < 1 || players > 4) players = 2;

    g_isHost = _strnicmp(mode, "host", 4) == 0;
    g_localSlot = g_isHost ? 0 : 1;
    g_localMask = (uint8_t)(1u << g_localSlot);   // legacy harness: one seat per instance
    g_localCount = 1;
    if (!OpenSocket()) return false;

    g_legacyMode = true;
    g_slotCount = players;
    for (int i = 0; i < kSlots; ++i) g_needInput[i] = (i < players);
    if (!g_isHost) {
        const char* ip = strchr(mode, ':');
        ip = ip ? ip + 1 : "127.0.0.1";
        g_legacyPeer.sin_family = AF_INET;
        g_legacyPeer.sin_port = htons((u_short)kBasePort);
        inet_pton(AF_INET, ip, &g_legacyPeer.sin_addr);
        g_haveLegacyPeer = true;
        printf("[net] JOIN -> %s:%d (bound %d), slot %d, delay %d\n",
               ip, kBasePort, g_port, g_localSlot, g_delay);
    } else {
        printf("[net] HOST on %d, slot %d, delay %d -- waiting for a joiner\n",
               g_port, g_localSlot, g_delay);
    }
    RingReset();
    g_armed = true;
    g_state = LAN_MATCH;
    return true;
}

// ---------------------------------------------------------------- lockstep tick
// TJ_WANDER=1: a test-harness "player" that walks the LOCAL fighter toward the item both
// players are contesting. It exists to answer the user's question -- "two players grab the
// same item on the same frame: exactly one pickup?" -- with measurement: an idle match never
// touches an item, and a randomly wandering one almost never does either.
//
// BOTH peers steer at the SAME item (the lowest-numbered active slot), so they converge on
// it from opposite sides and arrive within a frame or two of each other.
//
// The stick-to-world mapping depends on the live camera, so instead of deriving it this
// hill-climbs: hold a heading for 12 frames, and if the distance to the target did not drop,
// rotate. It converges in a second or so and needs no camera maths. This only ever shapes
// the local slot's own input BEFORE that input goes on the wire, so as far as the simulation
// is concerned it is an ordinary button press and lockstep is unaffected.
static int g_wander = -1;
static const uint32_t kFighterStride = 0x7080, kFighterBase = 0x4E0, kFighterPos = 0x6F90;
static const uint32_t kLevelPtrOff = 0x1C8EC, kItemArrOff = 0x2068, kItemStrideB = 0x1414;

static bool ContestedItemPos(float* out) {
    uint32_t master = *(volatile uint32_t*)(uintptr_t)0x15C470C;
    if (master < 0x04000000u || master >= 0x10000000u) return false;
    uint32_t level = *(volatile uint32_t*)(uintptr_t)(master + kLevelPtrOff);
    if (level < 0x04000000u || level + 0x41B18u >= 0x10000000u) return false;
    const uint8_t* L = (const uint8_t*)(uintptr_t)level;
    for (uint32_t i = 0; i < 50; ++i) {
        const uint8_t* rec = L + kItemArrOff + i * kItemStrideB;
        if (rec[0x1408] != 3) continue;                 // 3 = settled and takeable
        memcpy(out, rec + 0x13D0, 12);
        return true;
    }
    return false;
}
static bool FighterPos(int slot, float* out) {
    uint32_t master = *(volatile uint32_t*)(uintptr_t)0x15C470C;
    if (master < 0x04000000u || master >= 0x10000000u) return false;
    memcpy(out, (const void*)(uintptr_t)(master + kFighterBase + (uint32_t)slot * kFighterStride
                                         + kFighterPos), 12);
    return true;
}
static void ApplyWander(NetInput& in, uint32_t frame, int slot) {
    if (g_wander < 0) {
        char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_WANDER");
        g_wander = e && *e ? atoi(e) : 0; free(e);
        if (g_wander < 0 || g_wander > 2) g_wander = 1;
        printf("[lan] TJ_WANDER %s\n", g_wander == 2 ? "2 (walk at the other fighter)"
                                     : g_wander == 1 ? "1 (walk at the contested item)" : "off");
    }
    if (!g_wander) return;
    static float ang = 0.0f, lastDist = 1e9f;
    static uint32_t nextTurn = 0;
    float ip[3], fp[3];
    // TJ_WANDER=2: steer at the OTHER fighter instead of at an item. Used with TJ_ITEMTEST,
    // which only places an item between the two of them once they are genuinely close --
    // this is what walks them together, using nothing but ordinary stick input.
    bool haveTarget = (g_wander == 2) ? FighterPos(slot ^ 1, ip) : ContestedItemPos(ip);
    if (haveTarget && FighterPos(slot, fp)) {
        float dx = ip[0] - fp[0], dz = ip[2] - fp[2];
        float d = sqrtf(dx * dx + dz * dz);
        if (frame >= nextTurn) {
            if (d >= lastDist - 0.05f) ang += 0.9f;      // not closing: try another heading
            lastDist = d;
            nextTurn = frame + 12;
        }
    } else {
        ang += 0.37f;                                     // nothing to chase: keep moving
        lastDist = 1e9f;
    }
    in.thumb[0] = (int16_t)(cosf(ang) * 30000.0f);
    in.thumb[1] = (int16_t)(sinf(ang) * 30000.0f);
    // ...and mash attack. Damage is what makes the arena drop FOOD (FUN_000317E0 spawns
    // items from a damaged fighter), and food is the thing players actually race for -- an
    // unattended match with no punches thrown produces nothing to contest.
    // A = JUMP, B = GRAB, X = KICK (X in mid-air = smash). In MEAT RUSH the bot must press
    // B: mashing A and X only jumps and smashes, which collects nothing, so a run driven that
    // way banks zero meat and reads as if the scoring code were broken.
    if (MeatRushActive()) {
        if (((frame >> 3) & 1) == 0) in.analog[1] = 255;  // B -- grab the meat
        return;
    }
    if (((frame >> 3) & 1) == 0) in.analog[0] = 255;      // A
    if (((frame >> 4) & 3) == 0) in.analog[2] = 255;      // X
}

// One window per seat this PC drives. PktInput already names its slot, so a second local
// player needs no new packet type -- just a second packet.
static void SendInputWindowFor(uint32_t frame, int mySlot) {
    PktInput p{}; Hdrs(p.h, P_INPUT);
    p.seed = g_isHost ? NetSyncGetSeed() : 0u;
    p.slot = (uint8_t)mySlot;
    p.need = frame;                       // tell the peers which frame WE are stuck on
    // Start the window at the OLDEST frame any peer says it still needs, not at our own
    // frame: after a burst of loss the other side can be many frames behind, and a window
    // anchored locally would never contain the input it is waiting for. Capped at 16 frames;
    // a peer further behind than that catches up over successive packets as its `need`
    // advances.
    uint32_t lo = frame;
    for (int s = 0; s < kSlots; ++s)
        if (g_peerNeed[s] != kNoNeed && g_peerNeed[s] < lo && frame - g_peerNeed[s] < kRing)
            lo = g_peerNeed[s];
    uint32_t hi = frame + (uint32_t)g_delay;
    if (hi - lo >= 16) lo = hi - 15;
    p.baseFrame = lo;
    uint32_t cnt = 0;
    for (uint32_t f = lo; f <= hi && cnt < 16; ++f) {
        NetInput out;
        if (!RingGet(mySlot, f, &out)) break;
        p.in[cnt++] = out;
    }
    if (!cnt) return;
    p.count = (uint8_t)cnt;
    if (g_legacyMode) { if (g_haveLegacyPeer) Send(&p, sizeof p, g_legacyPeer); }
    else SendToPeers(&p, sizeof p);
}

static void SendInputWindow(uint32_t frame) {
    for (int j = 0; j < g_localCount; ++j) {
        int slot = LocalSlotAt(j);
        if (slot >= 0) SendInputWindowFor(frame, slot);
    }
}

bool NetTickBegin(uint32_t frame) {
    if (!g_armed) return true;
    FlushHeld();
    PumpRecv();

    // Schedule our own input D frames ahead -- ONCE. An input that has been sent is
    // immutable: while stalled we retry this function many times per frame, and re-sampling
    // would rewrite a slot the peer may have already received and acted on, so the two
    // peers would disagree about what was pressed. Sample only when the slot is empty.
    // ...for EVERY seat this PC drives. Local player j reads pad j; the seat they sit in is
    // whatever the lobby gave them, so the pad index and the seat index are unrelated.
    uint32_t sched = frame + (uint32_t)g_delay;
    for (int j = 0; j < g_localCount; ++j) {
        int slot = LocalSlotAt(j);
        if (slot < 0) continue;
        NetInput probe;
        if (RingGet(slot, sched, &probe)) continue;
        NetInput local{};
        GetLocalPadForNet(&local, j);
        ApplyWander(local, sched, slot);
        RingPut(slot, sched, local);
    }

    // ...and send a WINDOW of our recent scheduled frames, not just the newest one.
    // Two reasons, both load-bearing: (a) the host schedules frames 0..D-1 before it has
    // learned the joiner's address (it only learns it from the joiner's first packet), so
    // a send-newest-only scheme leaves a permanent hole and BOTH peers stall forever at
    // frame D -- observed exactly that; (b) UDP drops a datagram, and resending the window
    // every frame is the cheapest possible retransmit.
    SendInputWindow(frame);

    // Barrier: every slot that a HUMAN drives must have an input for THIS frame. CPU and
    // empty slots read a deterministic neutral pad -- they are driven by the identically
    // seeded AI, and waiting on them would deadlock.
    for (int s = 0; s < kSlots; ++s) {
        if (!g_needInput[s]) { memset(&g_cur[s], 0, sizeof g_cur[s]); continue; }
        if (!RingGet(s, frame, &g_cur[s])) {
            ++g_stallFrames;
            return false;                  // caller skips the whole update
        }
    }
    return true;
}

void NetTickEnd(uint32_t) {}

const NetInput* NetPad(int port) {
    if (!g_armed || port < 0 || port >= kSlots) return nullptr;
    return &g_cur[port];
}

void NetStatus(char* out, int cap) {
    if (g_state == LAN_OFF) { if (cap) out[0] = 0; return; }
    _snprintf_s(out, (size_t)cap, _TRUNCATE,
                " net=[%s slot%d d%d st=%d sent=%u recv=%u drop=%u stall=%u rtt=%ums f=%u]",
                g_isHost ? "host" : "peer", g_localSlot, g_delay, (int)g_state,
                g_sent, g_recv, g_dropped, g_stallFrames, g_rttMs, g_lastRemote);
    g_stallFrames = 0; g_sent = 0; g_recv = 0; g_dropped = 0;
}

// ---------------------------------------------------------------- session API
LanState LanGetState() { return g_state; }
bool LanIsHost() { return g_isHost; }
int  LanLocalSlot() { return g_localSlot; }
int  LanLocalCount() { return g_localCount; }
int  LanLocalSlotAt(int localIndex) { return LocalSlotAt(localIndex); }
int  LanSlotCount() { return kSlots; }
const LanSlotInfo* LanSlot(int i) { return (i >= 0 && i < kSlots) ? &g_slots[i] : nullptr; }
const char* LanStatusLine() { return g_status; }
LanNak LanLastNak() { return g_lastNak; }
const char* LanNakText() {
    // Localized like the LAN screens they appear on: Arabic when the player has chosen it,
    // the English literal otherwise (which is also what makes this switch readable).
    auto L = [](uint16_t id, const char* en) {
        if (ArabicEnabled()) if (const char* a = ArabicText(id)) return a;
        return en;
    };
    switch (g_lastNak) {
    case NAK_FULL:     return L(0x240, "THAT GAME IS FULL");
    // Reachable now only when the SIM CONTRACT differs — a real gameplay-affecting mismatch,
    // not merely a different binary, which cross-platform play makes routine.
    case NAK_BUILD:    return L(0x241, "THAT PLAYER HAS A DIFFERENT GAME VERSION");
    case NAK_DATA:     return L(0x242, "THAT PLAYER HAS DIFFERENT GAME FILES");
    // The player sees this when the two PCs are on different releases. Say what to DO about it
    // -- "incompatible LAN protocol" is true and completely useless to somebody in a lobby.
    case NAK_PROTO:    return L(0x243, "DIFFERENT VERSION - UPDATE BOTH PCS");
    case NAK_INMATCH:  return L(0x244, "THAT GAME HAS ALREADY STARTED");
    case NAK_PASSWORD: return L(0x245, "WRONG PASSWORD");
    default:           return "";
    }
}
const char* LanGetName() { return g_myName; }

#ifndef _WIN32
const char* UserDataDir();           // file_io.cpp -- the writable settings/save dir
#endif
static void IniPathLan(char* buf, size_t n) {
#ifdef _WIN32
    GetModuleFileNameA(nullptr, buf, (DWORD)n);
    char* s = strrchr(buf, '\\'); if (s) s[1] = 0;
    strcat_s(buf, n, "tomjerry.ini");
#else
    // The exe dir is Android's read-only native-lib dir, so the name lives in the same
    // tomjerry.ini every other settings screen uses (audio_ui/fe_menu: UserDataDir()).
    _snprintf_s(buf, n, _TRUNCATE, "%s\\tomjerry.ini", UserDataDir());
#endif
}
void LanSetName(const char* name) {
    SanitizeName(g_myName, sizeof g_myName, name);
    char ini[MAX_PATH]; IniPathLan(ini, sizeof ini);
    WritePrivateProfileStringA("Player", "Name", g_myName, ini);
    // One name is typed per machine and the other people sitting at it are derived from it, so
    // renaming has to re-derive every seat this PC owns -- otherwise "SHIHAB 2" keeps the old
    // player's surname after the first one renames themselves.
    if (g_isHost) {
        for (int j = 0; j < g_localCount; ++j) {
            int slot = LocalSlotAt(j);
            if (slot >= 0) LocalPlayerName(g_slots[slot].name, 16, g_myName, j);
        }
        PublishSlots(); SendLobbyState();
    } else if (g_state == LAN_LOBBY) {
        for (int j = 0; j < g_localCount; ++j) {
            int slot = LocalSlotAt(j);
            if (slot < 0) continue;
            PktIntent p{}; Hdrs(p.h, P_INTENT); p.sessionId = g_sessionId;
            p.slot = (uint8_t)slot; p.what = IN_NAME;
            LocalPlayerName(p.name, 16, g_myName, j);
            Send(&p, sizeof p, g_peers[0].addr);
        }
    }
}
static void LoadName() {
    char ini[MAX_PATH]; IniPathLan(ini, sizeof ini);
    char v[32] = "";
    GetPrivateProfileStringA("Player", "Name", "", v, sizeof v, ini);
    if (!v[0]) {
#ifdef _WIN32
        DWORD n = sizeof v; GetComputerNameA(v, &n);
#else
        if (gethostname(v, sizeof v) == 0) v[sizeof v - 1] = 0;
        else v[0] = 0;                       // SanitizeName falls back to "PLAYER"
#endif
    }
    SanitizeName(g_myName, sizeof g_myName, v);
}

// TJ_LAN_HASHDUMP=1 — compute and print the contract at STARTUP rather than waiting for the
// first LanOpen. ComputeHashes is deliberately lazy (it scans 453 asset files), which means a
// machine that never opens the LAN screen never logs its hashes — and comparing two machines
// then requires driving BOTH through their menus. This makes "do these two peers agree?" a
// one-line answer on each side. Off by default; costs nothing when unset.
void LanContractDump() {
    char* e = nullptr; size_t n = 0; _dupenv_s(&e, &n, "TJ_LAN_HASHDUMP");
    const bool on = e && *e && atoi(e) != 0; free(e);
    if (on) ComputeHashes();
}

bool LanOpen(bool host, const char* gameName, const char* password) {
    ComputeHashes();
    LoadName();
    if (!OpenSocket()) return false;
    g_lastNak = NAK_NONE;
    g_gameN = 0;
    g_haveCfg = false;
    if (host) {
        g_isHost = true;
        g_sessionId = Fnv32(&g_port, 4, Fnv32(g_myName, strlen(g_myName), Now()));
        if (!g_sessionId) g_sessionId = 1;
        SanitizeName(g_gameName, sizeof g_gameName, gameName && *gameName ? gameName : g_myName);
        g_pwHash = (password && *password) ? Fnv32(password, strlen(password), 0x9E3779B9u) : 0;
        if (!g_pwHash && password && *password) g_pwHash = 1;
        LobbyReset(true);
        PublishSlots();
        g_state = LAN_LOBBY;
        printf("[lan] HOSTING '%s' session=%08X on %d%s\n", g_gameName, g_sessionId, g_port,
               g_pwHash ? " (password)" : "");
        Status("%s %d%s", L(0x254, "HOSTING ON PORT"), g_port,
           g_pwHash ? (ArabicEnabled() ? " -" : " - LOCKED") : "");
    } else {
        g_isHost = false;
        LobbyReset(false);
        g_state = LAN_BROWSE;
        Status("%s", L(0x253, "SEARCHING FOR GAMES..."));
    }
    g_tBeacon = g_tPing = g_tProbe = 0;
    return true;
}

void LanClose(const char* why) {
    if (g_state == LAN_OFF) return;
    if (g_sock != INVALID_SOCKET && g_sessionId) {
        PktBye b{}; Hdrs(b.h, P_BYE); b.sessionId = g_sessionId; b.slot = (uint8_t)g_localSlot;
        if (g_isHost) SendToPeers(&b, sizeof b);
        else if (g_peers[0].active) Send(&b, sizeof b, g_peers[0].addr);
    }
    Disarm(why);
    g_state = LAN_OFF;
    g_sessionId = 0; g_gameN = 0; g_haveCfg = false;
    memset(g_peers, 0, sizeof g_peers);
    memset(g_slotKind, 0, sizeof g_slotKind);
    printf("[lan] session closed (%s)\n", why);
}

int LanGameCount() { return g_gameN; }
const LanGameInfo* LanGameAt(int i) { return (i >= 0 && i < g_gameN) ? &g_games[i] : nullptr; }

static void SendJoinReq() {
    PktJoinReq q{}; Hdrs(q.h, P_JOIN_REQ);
    q.sessionId = g_sessionId; q.buildHash = g_buildHash; q.dataHash = g_dataHash;
    q.moduleHash = g_moduleHash; q.patchFp = g_patchFp;
    q.pwHash = g_joinPwHash;
    memcpy(q.name, g_myName, 16);
    q.players = (uint8_t)GetLocalPlayerCount();
    Send(&q, sizeof q, g_peers[0].addr);
}

static bool JoinAddr(uint32_t ip, uint16_t port, uint32_t sessionId, const char* password) {
    memset(g_peers, 0, sizeof g_peers);
    g_peers[0].active = true;
    g_peers[0].addr.sin_family = AF_INET;
    g_peers[0].addr.sin_addr.s_addr = ip;
    g_peers[0].addr.sin_port = port;
    g_sessionId = sessionId;
    g_joinPwHash = (password && *password) ? Fnv32(password, strlen(password), 0x9E3779B9u) : 0;
    if (!g_joinPwHash && password && *password) g_joinPwHash = 1;
    g_state = LAN_JOINING;
    g_lastNak = NAK_NONE;
    g_tJoin = 0;
    SendJoinReq();
    Status("%s", L(0x255, "JOINING..."));
    return true;
}
bool LanJoinGame(int i, const char* password) {
    const LanGameInfo* g = LanGameAt(i);
    if (!g) return false;
    return JoinAddr(g->ip, g->port, g->sessionId, password);
}
// Accepts a dotted IPv4 address OR a host name -- typing "192.168.1.34" on a pad grid is
// tedious and error-prone, and on a home LAN the other PC's name is usually what people know.
// The grid is upper-case only, which is fine: DNS and NetBIOS name lookups are case-insensitive.
bool LanJoinIp(const char* host, const char* password) {
    if (!host || !*host) { Status("%s", L(0x27C, "ENTER AN ADDRESS OR PC NAME")); return false; }
    in_addr a{};
    if (inet_pton(AF_INET, host, &a) != 1) {
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
            printf("[lan] cannot resolve '%s'\n", host);
            Status("%s %s", L(0x27D, "CANNOT FIND"), host);
            return false;
        }
        a = ((sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
        printf("[lan] resolved '%s' -> %s\n", host, inet_ntoa(a));
    }
    // Try the whole port range: the other PC takes the first free port in it, which is 27100
    // unless something else there already had it.
    memset(g_peers, 0, sizeof g_peers);
    g_peers[0].active = true;
    g_peers[0].addr.sin_family = AF_INET;
    g_peers[0].addr.sin_addr = a;
    g_peers[0].addr.sin_port = htons((u_short)kBasePort);
    g_sessionId = 0;                          // 0 = "whatever session answers over there"
    g_joinPwHash = (password && *password) ? Fnv32(password, strlen(password), 0x9E3779B9u) : 0;
    if (!g_joinPwHash && password && *password) g_joinPwHash = 1;
    g_state = LAN_JOINING;
    g_lastNak = NAK_NONE;
    g_tJoin = 0;
    // A PROBE to every port on that host makes it answer with a unicast beacon, which both
    // populates the browser row and tells us which port it is really on.
    PktProbe pr{}; Hdrs(pr.h, P_PROBE); pr.nonce = Now();
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_addr = a;
    for (int i = 0; i < kPortSpan; ++i) {
        to.sin_port = htons((u_short)(kBasePort + i));
        Send(&pr, sizeof pr, to);
    }
    SendJoinReq();
    Status("%s %s", L(0x27E, "CONNECTING TO"), inet_ntoa(a));
    return true;
}

// Every lobby action names the seat it applies to. Two people at one PC each drive their own
// seat, so "my seat" is no longer a single answer -- the no-argument forms below keep working
// and mean the primary seat (this PC's player 1).
void LanSetReadyFor(int slot, bool ready) {
    if (!IsLocalSlot(slot)) return;
    g_slots[slot].ready = ready ? 1 : 0;
    if (g_isHost) { PublishSlots(); SendLobbyState(); return; }
    if (g_state != LAN_LOBBY) return;
    PktIntent p{}; Hdrs(p.h, P_INTENT); p.sessionId = g_sessionId;
    p.slot = (uint8_t)slot; p.what = IN_READY; p.arg = ready ? 1 : 0;
    Send(&p, sizeof p, g_peers[0].addr);
}
void LanSetReady(bool ready) { LanSetReadyFor(g_localSlot, ready); }

static void SendSeatIntent(int slot, uint8_t what, uint8_t arg) {
    if (g_state != LAN_LOBBY || g_isHost) return;
    PktIntent p{}; Hdrs(p.h, P_INTENT); p.sessionId = g_sessionId;
    p.slot = (uint8_t)slot; p.what = what; p.arg = arg;
    Send(&p, sizeof p, g_peers[0].addr);
}
static void SendSimpleIntent(uint8_t what, uint8_t arg) { SendSeatIntent(g_localSlot, what, arg); }
// Character/costume picks are validated against the LOCAL unlock tables before they are
// offered, and again by the host, so a LAN pick can never be something the engine rejects.
static const int kCharCount = 11, kCostumeCount = 6;
void LanCycleCharFor(int slot, int delta) {
    if (!IsLocalSlot(slot)) return;
    LanSlotInfo& s = g_slots[slot];
    int c = s.charId;
    for (int i = 0; i < kCharCount; ++i) {
        c = (c + delta + kCharCount) % kCharCount;
        if (*(volatile uint8_t*)(uintptr_t)(0x16A23A + c)) break;
    }
    s.charId = (uint8_t)c;
    // Characters do not all have the same number of costumes (5 down to 1), so a costume
    // index that was legal for the last character may not be for this one.
    if (s.costume >= LanCostumeCount(s.charId)) s.costume = 0;
    if (g_isHost) { SendLobbyState(); } else SendSeatIntent(slot, IN_CHAR, s.charId);
}
void LanCycleChar(int delta) { LanCycleCharFor(g_localSlot, delta); }
// TEAM LETTERS. A player owns their own letter; the host owns every other seat's, which is
// how a host arranges 2v2 without waiting on anyone. Letters are cosmetic-plus-grouping: the
// retail commit compacts whatever it is given, and there is no friendly-fire gate anywhere in
// the damage path -- a shared letter only removes knockback and the impact FX (FUN_000409B0).
void LanCycleTeamFor(int slot, int delta) {
    if (!IsLocalSlot(slot)) return;
    LanSlotInfo& s = g_slots[slot];
    s.team = (uint8_t)((s.team + delta + 4) & 3);
    if (g_isHost) { SendLobbyState(); } else SendSeatIntent(slot, IN_TEAM, s.team);
}
void LanCycleTeam(int delta) { LanCycleTeamFor(g_localSlot, delta); }
void LanHostCycleTeam(int slot, int delta) {
    if (!g_isHost || slot < 0 || slot >= kSlots || !g_slotKind[slot]) return;
    g_slots[slot].team = (uint8_t)((g_slots[slot].team + delta + 4) & 3);
    PublishSlots(); SendLobbyState();
}
// The host may also drive a remote player's CHARACTER for an empty/CPU seat.
void LanHostCycleChar(int slot, int delta) {
    if (!g_isHost || slot < 0 || slot >= kSlots || g_slotKind[slot] != 2) return;
    int c = g_slots[slot].charId;
    for (int i = 0; i < kCharCount; ++i) {
        c = (c + delta + kCharCount) % kCharCount;
        if (*(volatile uint8_t*)(uintptr_t)(0x16A23A + c)) break;
    }
    g_slots[slot].charId = (uint8_t)c;
    if (g_slots[slot].costume >= LanCostumeCount(c)) g_slots[slot].costume = 0;
    PublishSlots(); SendLobbyState();
}
// HOW MANY COSTUMES A CHARACTER HAS IS A STATIC CEILING, NOT A SAVE FACT. The 0x74-byte save
// blob at 0x16A1F8 opens with 11 six-byte records -- bytes [0..4] the costume ids this PC has
// EARNED, byte [5] how many -- so it differs between two machines, and driving the lobby from
// it would let one peer offer a costume the other cannot resolve. The read-only table at
// 0x169910 (same stride, count in byte +5) is the ceiling and is identical everywhere: TOM 5,
// JERRY 5, SPIKE/BUTCH/NIBBLES/DUCKLING 3, the other five 1. The ids are contiguous 0..n-1
// (every award in FUN_0002DA50's tables lands inside that range), so the INDEX IS THE ID and
// the launch can write it straight into FE+0x4AC without consulting any save.
int LanCostumeCount(int charId) {
    if (charId < 0 || charId >= kCharCount) return 1;
    int n = *(volatile uint8_t*)(uintptr_t)(0x169915 + (uint32_t)charId * 6);
    return (n >= 1 && n <= 5) ? n : 1;
}
// A PLAYER MAY ONLY PICK WHAT THEY HAVE UNLOCKED. The ceiling above says what EXISTS (and so
// what can be drawn); this says what this PC's save has earned, which is what the lobby is
// allowed to cycle through. Defined in lan_match.cpp, where the blob snapshot lives.
int LanCostumeUnlocked(int charId);
void LanCycleCostumeFor(int slot, int delta) {
    if (!IsLocalSlot(slot)) return;
    LanSlotInfo& s = g_slots[slot];
    int n = LanCostumeUnlocked(s.charId);
    if (n < 2) return;
    s.costume = (uint8_t)((s.costume + delta + n) % n);
    if (g_isHost) { SendLobbyState(); } else SendSeatIntent(slot, IN_COSTUME, s.costume);
}
void LanCycleCostume(int delta) { LanCycleCostumeFor(g_localSlot, delta); }
void LanHostCycleCostume(int slot, int delta) {
    if (!g_isHost || slot < 0 || slot >= kSlots || g_slotKind[slot] != 2) return;
    LanSlotInfo& s = g_slots[slot];
    int n = LanCostumeUnlocked(s.charId);
    if (n < 2) return;
    s.costume = (uint8_t)((s.costume + delta + n) % n);
    PublishSlots(); SendLobbyState();
}
void LanHostSetRules(uint32_t timeLimit, uint8_t rounds, uint8_t difficulty, uint8_t meat) {
    if (!g_isHost) return;
    g_ruleTime = timeLimit;
    g_ruleRounds = (rounds == 1 || rounds == 3 || rounds == 5) ? rounds : 1;
    g_ruleDiff = difficulty <= 2 ? difficulty : 1;
    g_ruleMeat = (meat == 0 || meat == 5 || meat == 10 || meat == 15 ||
                  meat == 20 || meat == 0xFF) ? meat : 0;
    SendLobbyState();
}
bool LanArenaSelectable(int arena) {
    if (arena < 0 || arena >= 13) return false;
    if (arena == 10 || arena == 12) return false;      // retail versus excludes both
    return *(volatile uint8_t*)(uintptr_t)(0x16A245 + arena) != 0;
}
uint8_t  LanConfigArena()  { return g_ruleArena; }
uint8_t  LanConfigMode()   { return g_ruleMode; }
uint8_t  LanConfigRounds() { return g_ruleRounds; }
uint8_t  LanConfigMeat()   { return g_ruleMeat; }
uint32_t LanConfigTime()   { return g_ruleTime; }
uint32_t LanPingMs()       { return g_rttMs; }
void LanHostSetArena(uint8_t arena) {
    if (g_isHost && LanArenaSelectable(arena)) { g_ruleArena = arena; SendLobbyState(); }
}
void LanHostSetMode(uint8_t mode)   { if (g_isHost) { g_ruleMode = mode <= 2 ? mode : 0; SendLobbyState(); } }
// THE HOST CAN REMOVE A PLAYER. Someone has to be able to: a pad dies, somebody wanders off,
// or a seat is simply in the way of starting. The seat is freed here and the peer is told with
// the same BYE it would have sent itself, so a 2-player PC losing one seat keeps the other.
void LanHostKick(int slot) {
    if (!g_isHost || slot <= 0 || slot >= kSlots) return;
    if (IsLocalSlot(slot)) return;                  // not for our own seats -- unplug instead
    if (g_slotKind[slot] != 1) return;              // only a human occupies a seat this way
    char who[16]; strncpy_s(who, g_slots[slot].name, _TRUNCATE);
    if (g_peers[slot].active) {
        PktBye b{}; Hdrs(b.h, P_BYE); b.sessionId = g_sessionId;
        b.slot = (uint8_t)slot; b.reason = 1;
        Send(&b, sizeof b, g_peers[slot].addr);
    }
    g_peers[slot].active = false;
    g_slotKind[slot] = 0;
    g_slots[slot].ready = 0; g_slots[slot].name[0] = 0;
    printf("[lan] host removed slot %d (%s)\n", slot, who);
    Status("%s %s", L(0x276, "WAS REMOVED"), who[0] ? who : "");
    PublishSlots(); SendLobbyState();
}

void LanHostToggleCpu(int slot) {
    if (!g_isHost || slot <= 0 || slot >= kSlots) return;
    if (g_peers[slot].active) return;               // a real player owns it
    g_slotKind[slot] = g_slotKind[slot] == 2 ? 0 : 2;
    g_slots[slot].ready = 0;
    _snprintf_s(g_slots[slot].name, 16, _TRUNCATE, g_slotKind[slot] == 2 ? "CPU" : "");
    PublishSlots(); SendLobbyState();
}
// Mirrors the retail guard FUN_000243F0. The TWO-DISTINCT-TEAMS rule became mandatory the
// moment the lobby let players pick a letter: with every seat on one letter the commit writes
// FE+0x500 = 1, every AI target lookup returns 0 and the round can never end.
// ALL FIVE ARE LOCALIZED, not just the common one. They share the lobby's single prompt
// slot -- the most-read line on the screen -- so leaving any of them English flips that line
// back to English at the exact moment the player needs to read it. (User-reported: "NEED AT
// LEAST TWO PLAYERS" sitting under a fully Arabic lobby.)
const char* LanStartRefusal() {
    if (!g_isHost || g_state != LAN_LOBBY) return L(0x260, "NOT THE HOST");
    int humans = 0, filled = 0;
    bool letter[4] = { false, false, false, false };
    for (int i = 0; i < kSlots; ++i) {
        if (!g_slotKind[i]) continue;
        ++filled;
        letter[g_slots[i].team & 3] = true;
        if (g_slotKind[i] != 1) continue;
        ++humans;
        if (!IsLocalSlot(i) && !g_slots[i].ready)
            return L(0x261, "WAITING FOR EVERYONE TO BE READY");
    }
    if (humans < 1) return L(0x262, "NEED AT LEAST ONE HUMAN PLAYER");
    if (filled < 2) return L(0x263, "NEED AT LEAST TWO PLAYERS");
    int teams = 0;
    for (int i = 0; i < 4; ++i) if (letter[i]) ++teams;
    if (teams < 2) return L(0x264, "NEED AT LEAST TWO DIFFERENT TEAMS");
    return nullptr;
}
bool LanHostCanStart() { return LanStartRefusal() == nullptr; }
void LanHostStart() {
    if (!LanHostCanStart()) return;
    BuildConfig(g_cfg);
    g_cfgCrc = LanConfigCrc(&g_cfg);
    g_haveCfg = true;
    memset(g_readySeen, 0, sizeof g_readySeen);
    for (int i = 0; i < kSlots; ++i) if (IsLocalSlot(i)) g_readySeen[i] = true;
    g_state = LAN_STARTING;
    g_tRetry = 0;
    Status("%s", L(0x256, "STARTING MATCH..."));
    printf("[lan] START match %08X seed=%u arena=%d rounds=%d\n",
           g_cfg.matchId, g_cfg.seed, g_cfg.arena, g_cfg.rounds);
}
void LanLeave() {
    if (g_state == LAN_OFF) return;
    if (!g_isHost && g_peers[0].active) {
        PktIntent p{}; Hdrs(p.h, P_INTENT); p.sessionId = g_sessionId;
        p.slot = (uint8_t)g_localSlot; p.what = IN_LEAVE;
        memcpy(p.name, g_myName, 16);
        Send(&p, sizeof p, g_peers[0].addr);
    }
    LanClose("left");
}

bool LanConsumeLaunch() {
    if (!g_launchPending) return false;
    g_launchPending = false;
    return true;
}
void LanMatchEnded() {
    if (g_legacyMode) return;                     // the harness never leaves the match
    Disarm("match ended");
    if (g_state == LAN_MATCH) {
        g_state = LAN_LOBBY;
        for (int i = 0; i < kSlots; ++i) g_slots[i].ready = 0;
        if (g_isHost) SendLobbyState();
        Status("%s", L(0x277, "BACK IN THE LOBBY"));
    }
}

// Can this peer honour a launch on its very next simulation step? Both peers must return
// the retail launch sentinel from the SAME lockstep frame, so BARRIER_READY is only sent
// once the frontend is sitting on a screen whose Update we hook (the LAN lobby, or the
// main menu in the headless harness) and is not mid-transition.
extern bool LanFrontendCanLaunch();               // lan_ui.cpp

// ---------------------------------------------------------------- pump
// A CONTROLLER PLUGGED IN DURING THE LOBBY SHOULD JOIN THE GAME. Seats used to be counted once
// at host/join time, so someone who picked up a pad after the lobby opened -- the normal way it
// happens, since a pad only claims a player number on its first press -- simply never appeared.
// Checked in the lobby only: seats cannot change once the match is arming, or the two peers
// would disagree about who is playing.
static void PollLocalPlayers() {
    if (g_state != LAN_LOBBY) return;
    int want = GetLocalPlayerCount();

    // A CONTROLLER UNPLUGGED IN THE LOBBY GIVES ITS SEAT BACK -- but not instantly. Wireless
    // pads drop for a second or two all the time, and evicting somebody because their batteries
    // hiccuped is worse than a seat that lingers. The seat is only released after the pad has
    // been gone continuously for the grace period; plugging back in inside that window is a
    // no-op. The HIGHEST local seat goes first, so the person who joined last is the one who
    // loses their place rather than an arbitrary one.
    static uint32_t goneSince = 0;
    if (want < g_localCount) {
        if (!goneSince) goneSince = Now();
        if (Now() - goneSince < 4000) return;
        goneSince = 0;
        while (g_localCount > want && g_localCount > 1) {
            int slot = LocalSlotAt(g_localCount - 1);
            if (slot < 0) break;
            printf("[lan] a controller was unplugged: releasing seat %d\n", slot);
            if (g_isHost) {
                g_peers[slot].active = false;
                g_slotKind[slot] = 0;
                g_slots[slot].ready = 0; g_slots[slot].name[0] = 0;
            } else {
                PktIntent p{}; Hdrs(p.h, P_INTENT); p.sessionId = g_sessionId;
                p.slot = (uint8_t)slot; p.what = IN_LEAVE;
                memcpy(p.name, g_slots[slot].name, 16);
                Send(&p, sizeof p, g_peers[0].addr);
            }
            g_localMask &= (uint8_t)~(1u << slot);
            --g_localCount;
        }
        g_localSlot = LocalSlotAt(0) < 0 ? g_localSlot : LocalSlotAt(0);
        Status("%s", L(0x278, "A CONTROLLER WAS UNPLUGGED"));
        if (g_isHost) { PublishSlots(); SendLobbyState(); }
        return;
    }
    goneSince = 0;

    if (want <= g_localCount) return;
    if (g_isHost) {
        int added = 0;
        for (int i = 0; i < kSlots && g_localCount < want; ++i) {
            if (g_peers[i].active || g_slotKind[i]) continue;
            g_peers[i].active = true; g_peers[i].slot = (uint8_t)i; g_peers[i].lastRecv = Now();
            g_localMask |= (uint8_t)(1u << i);
            ++g_localCount; ++added;
            g_slotKind[i] = 1;
            g_slots[i].ready = 0;
        }
        if (added) {
            for (int j = 0; j < g_localCount; ++j) {
                int slot = LocalSlotAt(j);
                if (slot >= 0) LocalPlayerName(g_slots[slot].name, 16, g_myName, j);
            }
            printf("[lan] a controller joined: this PC now drives %d seat(s)\n", g_localCount);
            PublishSlots(); SendLobbyState();
        }
    } else {
        // Ask the host for another seat. JOIN_REQ is idempotent and now tops up, and the reply
        // is handled in the lobby by OnJoinRsp. Rate-limited: it rides the existing retry.
        static uint32_t lastAsk = 0;
        if (Now() - lastAsk < 500) return;
        lastAsk = Now();
        SendJoinReq();
    }
}

void LanPump() {
    if (g_state == LAN_OFF) return;
    FlushHeld();
    PumpRecv();
    PollLocalPlayers();
    uint32_t now = Now();

    // age the browser table
    for (int i = 0; i < g_gameN;) {
        if (now - g_games[i].ageMs > kGameTtlMs) g_games[i] = g_games[--g_gameN];
        else ++i;
    }

    if (g_isHost && now - g_tBeacon >= kBeaconMs) {
        g_tBeacon = now;
        PktBeacon b{}; FillBeacon(b);
        Broadcast(&b, sizeof b);
    }
    if (!g_isHost && g_state == LAN_BROWSE && now - g_tProbe >= 1000) {
        g_tProbe = now;
        PktProbe p{}; Hdrs(p.h, P_PROBE); p.nonce = now;
        Broadcast(&p, sizeof p);
    }
    if (g_state == LAN_JOINING && now - g_tJoin >= 300) {
        g_tJoin = now;
        static int tries = 0;
        if (++tries > 20) { tries = 0; g_state = LAN_BROWSE; Status("%s", L(0x279, "NO ANSWER FROM THAT GAME")); }
        else SendJoinReq();
    }
    // PING/PONG -> RTT -> automatic input delay, frozen at GO.
    if (now - g_tPing >= kPingMs) {
        g_tPing = now;
        PktPing p{}; Hdrs(p.h, P_PING); p.stamp = now; p.slot = (uint8_t)g_localSlot;
        if (g_state == LAN_BROWSE) {
            for (int i = 0; i < g_gameN; ++i) {
                sockaddr_in to{}; to.sin_family = AF_INET;
                to.sin_addr.s_addr = g_games[i].ip; to.sin_port = g_games[i].port;
                Send(&p, sizeof p, to);
            }
        } else if (g_isHost) SendToPeers(&p, sizeof p);
        else if (g_peers[0].active) Send(&p, sizeof p, g_peers[0].addr);
        if (!g_delayForced && g_state == LAN_LOBBY) {
            // D = ceil(rtt_frames) + 1, clamped -- one 60 Hz frame is 16.6 ms.
            int d = (int)((g_rttMs * 60 + 999) / 1000) + 1;
            if (d < 2) d = 2; if (d > 8) d = 8;
            g_delay = d;
        }
    }
    if (g_isHost && g_state == LAN_LOBBY && now - g_tRetry >= 1000) {
        g_tRetry = now;
        SendLobbyState();
        // ⚠ A LOCAL SEAT CAN NEVER TIME OUT. The host's own extra players are peer entries like
        // any other, but no datagram ever arrives from this machine to refresh them, so the TTL
        // expired and the host evicted its own player 2 a few seconds into every lobby. Only
        // seats reached over the network are subject to the timeout.
        for (int i = 1; i < kSlots; ++i)
            if (g_peers[i].active && !IsLocalSlot(i) && now - g_peers[i].lastRecv > kPeerTtlMs) {
                printf("[lan] slot %d timed out\n", i);
                g_peers[i].active = false; g_slotKind[i] = 0;
                g_slots[i].ready = 0; g_slots[i].name[0] = 0;
                Status("%s", L(0x27A, "A PLAYER TIMED OUT"));
                PublishSlots(); SendLobbyState();
            }
    }
    // START handshake (reliable by retry; every peer answers with ITS OWN config CRC so a
    // mismatch aborts loudly instead of starting a doomed match).
    if (g_state == LAN_STARTING && now - g_tRetry >= kRetryMs) {
        g_tRetry = now;
        if (g_isHost) {
            PktStart s{}; Hdrs(s.h, P_START); s.sessionId = g_sessionId;
            s.cfg = g_cfg; s.crc = g_cfgCrc;
            SendToPeers(&s, sizeof s);
            bool all = true;
            for (int i = 0; i < kSlots; ++i)
                if (g_slotKind[i] == 1 && !IsLocalSlot(i) && !g_readySeen[i]) all = false;
            if (all && LanFrontendCanLaunch()) {
                PktGo g{}; Hdrs(g.h, P_GO); g.sessionId = g_sessionId; g.matchId = g_cfg.matchId;
                for (int k = 0; k < 4; ++k) SendToPeers(&g, sizeof g);
                ArmLockstep(g_cfg);
            }
        } else if (g_haveCfg && LanFrontendCanLaunch()) {
            // ONE READY PER SEAT, NOT PER MACHINE. The host's GO gate demands g_readySeen[i]
            // for EVERY non-local human seat, and OnJoinReq marks every granted seat kind 1 --
            // but this used to send a single packet naming only g_localSlot, so the second
            // seat of a two-pad joiner was never marked ready. `all` never became true, GO was
            // never sent, and both machines sat in LAN_STARTING forever showing "STARTING
            // MATCH...". That is THREE PLAYERS ON TWO MACHINES: a second, entirely separate
            // hang from the 3-machine input deadlock, reachable from the normal UI because the
            // lobby's own READY toggle writes a DIFFERENT flag (g_slots[i].ready), so the
            // host's START button is enabled and the freeze lands after the player commits.
            // Session 22 verified the mirror image (extra seats on the HOST, which
            // LanHostStart pre-marks), so this direction was never exercised.
            // PktReady is unchanged, so an old host simply sets the same bit twice: no
            // kProtoVer bump.
            for (int j = 0; j < g_localCount; ++j) {
                int seat = LocalSlotAt(j);
                if (seat < 0) continue;
                PktReady r{}; Hdrs(r.h, P_READY); r.sessionId = g_sessionId;
                r.matchId = g_cfg.matchId; r.crc = LanConfigCrc(&g_cfg);
                r.slot = (uint8_t)seat;
                Send(&r, sizeof r, g_peers[0].addr);
            }
        }
    }
    // GO IS THE ONE PACKET NOBODY ANSWERS, AND IT WAS SENT EXACTLY ONCE. Four copies went out
    // back to back and the host then armed and left LAN_STARTING, so nothing ever sent it
    // again -- and four back-to-back copies is precisely what a single loss BURST swallows
    // (the harness injects bursts of five). Measured: the joiner sat in LAN_STARTING for the
    // whole run while the host stalled at lockstep frame 2 forever, i.e. both PCs hang. So the
    // host keeps re-sending GO until each peer's first INPUT packet proves it armed. This is
    // free: OnGo ignores a duplicate once armed, and arming is frame-relative (the lockstep
    // index resets to 0 at every arm), so a peer that arms late is still in step.
    if (g_isHost && !g_legacyMode && g_armed && now - g_tGo >= kRetryMs) {
        g_tGo = now;
        bool missing = false;
        for (int i = 0; i < kSlots; ++i)
            if (!IsLocalSlot(i) && g_slotKind[i] == 1 && g_peers[i].active && !g_peerInputSeen[i])
                missing = true;
        if (missing) {
            PktGo g{}; Hdrs(g.h, P_GO); g.sessionId = g_sessionId; g.matchId = g_cfg.matchId;
            SendToPeers(&g, sizeof g);
        }
    }
}

}  // namespace tj::hybrid
