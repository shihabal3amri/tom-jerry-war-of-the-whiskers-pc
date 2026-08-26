// See apk_build.h for what this is and why v1 signing is the right choice here.
//
// THE GAME PACK. Every game file goes into ONE uncompressed zip entry, assets/game.pak, which
// the app unpacks on first launch (native_main.cpp). Two reasons it is a pack and not loose
// assets: AAssetManager cannot reliably enumerate directories, so the app would need an index
// anyway; and a single STORED entry keeps THIS file to byte-copying plus SHA-256, with no
// deflate implementation anywhere. Game textures and audio barely compress, so it costs
// nothing worth having.
//
//   "TJPK", u32 version=1, u32 fileCount,
//   fileCount x { u16 pathLen, path ('/'-separated, relative), u64 size },
//   then every file's bytes back to back, in the same order.
//
// ⚠ EVERY TEMPLATE ENTRY MUST BE STORED. A v1 signature digests each entry's UNCOMPRESSED
// bytes; storing them means the bytes we copy are the bytes we hash. port/tools/build_apk.ps1
// -Template guarantees it (apk_store_all.py re-packs even AndroidManifest.xml, which aapt2
// always deflates), and ReadTemplate below REFUSES anything deflated rather than writing an
// apk whose signature would silently not verify.
#include "setup/apk_build.h"

#include <windows.h>
#include <wincrypt.h>
#include <vector>
#include <string>
#include <objbase.h>   // CoCreateGuid
#include <string>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace tj::setup {
namespace {

// The PFX is a local cache of a locally generated key; the password only stops the
// file being usable by accident, and is not protecting anything from anyone.
static const wchar_t* const kPfxPassword = L"tjwow";

// WHAT A USABLE SIGNING IDENTITY IS. Preferred form is a PFX (key + certificate in one file,
// portable, self-contained). But PFX export is the ONE step in this whole sequence that a
// hostile environment can refuse while everything else succeeds -- an antivirus heuristic that
// treats "export a private key" as credential theft, a policy that forbids it, a CSP that
// cannot produce it. When that happens the key and certificate are still perfectly good; only
// the PKCS#12 packaging failed. So there is a second form: the certificate's DER bytes beside
// the private key as a raw PRIVATEKEYBLOB, which needs only CryptExportKey and touches none of
// the PKCS#12 machinery that was refused.
// ⚠ BOTH FORMS ARE FILES, and the first attempt at this got that wrong. It kept the key in its
// CAPI container and remembered only the container NAME -- and a container is managed state
// that antivirus, profile cleanup and roaming can all remove, on precisely the machines where
// this fallback triggers. One field report came back as CRYPT_E_NO_KEY_PROPERTY: the container
// had gone, and because nothing revalidated it, the leftover .cer/.csp pair failed FOREVER with
// no way back. A file is as durable as the PFX would have been.
// Both forms sign identically and keep the SAME certificate across installs, which is the only
// property Android cares about for updating an app in place.
struct SigningKey {
    std::vector<uint8_t> pfx;        // form 1: the whole identity in one blob
    std::vector<uint8_t> cerDer;     // form 2: the certificate...
    std::vector<uint8_t> keyBlob;    // ...and its private key as a raw PRIVATEKEYBLOB
    std::string          note;       // why form 2 was used, kept beside the key on disk
    bool Empty() const { return pfx.empty() && (cerDer.empty() || keyBlob.empty()); }
    bool UsesPfx() const { return !pfx.empty(); }
};

// The CSP is named EXPLICITLY everywhere, and that is not cosmetic. Passing nullptr means "the
// default provider for PROV_RSA_AES", which is a registry setting a third-party CSP (antivirus,
// smart-card middleware, an enterprise agent) is free to redirect. Create the key under one
// provider and let PFXExportCertStoreEx look for it under another and the export fails to find
// a key that plainly exists -- with nothing useful in GetLastError, because nothing errored.
static const wchar_t* const kCsp = MS_ENH_RSA_AES_PROV_W;

// TJ_APK_NO_PFX=1 forces the export to "fail" so the container-backed path can be exercised
// on a machine where the normal one works. The fallback only ever runs where the ordinary
// route already failed, which means it is exactly the code no developer's machine reaches --
// so there has to be a way to reach it deliberately, or it ships untested.
bool ForceExportFailure() {
    char v[8] = {};
    return GetEnvironmentVariableA("TJ_APK_NO_PFX", v, sizeof v) > 0 && v[0] && v[0] != '0';
}


// ---------------------------------------------------------------- little-endian writers
void Put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (i * 8))); }
uint16_t Get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// ---------------------------------------------------------------- CRC32 (zip polynomial)
struct Crc32 {
    static const uint32_t* Table() {
        static uint32_t t[256];
        static bool built = false;
        if (!built) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            built = true;
        }
        return t;
    }
    uint32_t v = 0xFFFFFFFFu;
    void Update(const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        const uint32_t* t = Table();
        for (size_t i = 0; i < n; ++i) v = t[(v ^ p[i]) & 0xFF] ^ (v >> 8);
    }
    uint32_t Final() const { return v ^ 0xFFFFFFFFu; }
};

// ---------------------------------------------------------------- SHA-256 + base64
struct Sha256 {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool Begin() {
        if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
        return CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash) != 0;
    }
    void Update(const void* d, size_t n) { if (hash && n) CryptHashData(hash, (const BYTE*)d, (DWORD)n, 0); }
    bool Final(std::vector<uint8_t>& out) {
        DWORD len = 32;
        out.resize(32);
        bool ok = hash && CryptGetHashParam(hash, HP_HASHVAL, out.data(), &len, 0);
        if (hash) CryptDestroyHash(hash);
        if (prov) CryptReleaseContext(prov, 0);
        hash = 0; prov = 0;
        return ok;
    }
    ~Sha256() { if (hash) CryptDestroyHash(hash); if (prov) CryptReleaseContext(prov, 0); }
};

bool Sha256Of(const void* data, size_t n, std::vector<uint8_t>& out) {
    Sha256 h;
    if (!h.Begin()) return false;
    h.Update(data, n);
    return h.Final(out);
}

std::string Base64(const std::vector<uint8_t>& raw) {
    DWORD n = 0;
    if (!CryptBinaryToStringA(raw.data(), (DWORD)raw.size(),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n)) return "";
    std::string s(n, '\0');
    if (!CryptBinaryToStringA(raw.data(), (DWORD)raw.size(),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &s[0], &n)) return "";
    s.resize(strlen(s.c_str()));
    return s;
}

// ---------------------------------------------------------------- a file being written
struct OutFile {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t pos = 0;
    bool Open(const std::wstring& path) {
        h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return h != INVALID_HANDLE_VALUE;
    }
    bool Write(const void* d, size_t n) {
        const uint8_t* p = (const uint8_t*)d;
        while (n) {
            DWORD put = 0;
            DWORD chunk = (DWORD)(n > (1u << 24) ? (1u << 24) : n);
            if (!WriteFile(h, p, chunk, &put, nullptr) || put == 0) return false;
            p += put; n -= put; pos += put;
        }
        return true;
    }
    bool Write(const std::vector<uint8_t>& v) { return v.empty() || Write(v.data(), v.size()); }
    // Patch a field already written (the pack's CRC, once its bytes are known).
    bool PatchAt(uint64_t at, const void* d, size_t n) {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)at;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
        DWORD put = 0;
        bool ok = WriteFile(h, d, (DWORD)n, &put, nullptr) && put == n;
        li.QuadPart = (LONGLONG)pos;
        SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
        return ok;
    }
    void Close() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    ~OutFile() { Close(); }
};

// ---------------------------------------------------------------- zip pieces
struct OutEntry {                 // what the central directory will need
    std::string name;
    uint32_t crc = 0;
    uint64_t size = 0;            // STORED: compressed == uncompressed
    uint64_t localOfs = 0;
};

void LocalHeader(std::vector<uint8_t>& v, const std::string& name, uint32_t crc, uint64_t size) {
    Put32(v, 0x04034b50);
    Put16(v, 20);                 // version needed
    Put16(v, 0);                  // flags — no data descriptor: sizes are known up front
    Put16(v, 0);                  // method 0 = STORED
    Put16(v, 0); Put16(v, 0);     // time, date
    Put32(v, crc);
    Put32(v, (uint32_t)size);
    Put32(v, (uint32_t)size);
    Put16(v, (uint16_t)name.size());
    Put16(v, 0);                  // extra len
    v.insert(v.end(), name.begin(), name.end());
}

// A STORED entry whose bytes are already in memory.
bool AddEntry(OutFile& out, std::vector<OutEntry>& dir, const std::string& name,
              const void* data, size_t n) {
    Crc32 c; c.Update(data, n);
    OutEntry e; e.name = name; e.crc = c.Final(); e.size = n; e.localOfs = out.pos;
    std::vector<uint8_t> hdr;
    LocalHeader(hdr, name, e.crc, n);
    if (!out.Write(hdr) || !out.Write(data, n)) return false;
    dir.push_back(e);
    return true;
}

bool WriteCentralDirectory(OutFile& out, const std::vector<OutEntry>& dir) {
    const uint64_t start = out.pos;
    std::vector<uint8_t> cd;
    for (const OutEntry& e : dir) {
        Put32(cd, 0x02014b50);
        Put16(cd, 20); Put16(cd, 20);
        Put16(cd, 0); Put16(cd, 0);
        Put16(cd, 0); Put16(cd, 0);
        Put32(cd, e.crc);
        Put32(cd, (uint32_t)e.size);
        Put32(cd, (uint32_t)e.size);
        Put16(cd, (uint16_t)e.name.size());
        Put16(cd, 0); Put16(cd, 0);
        Put16(cd, 0); Put16(cd, 0);
        Put32(cd, 0);
        Put32(cd, (uint32_t)e.localOfs);
        cd.insert(cd.end(), e.name.begin(), e.name.end());
    }
    if (!out.Write(cd)) return false;
    std::vector<uint8_t> eocd;
    Put32(eocd, 0x06054b50);
    Put16(eocd, 0); Put16(eocd, 0);
    Put16(eocd, (uint16_t)dir.size());
    Put16(eocd, (uint16_t)dir.size());
    Put32(eocd, (uint32_t)cd.size());
    Put32(eocd, (uint32_t)start);
    Put16(eocd, 0);
    return out.Write(eocd);
}

// ---------------------------------------------------------------- the template
struct TplEntry {
    std::string name;
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

bool ReadTemplate(const uint8_t* apk, size_t n, std::vector<TplEntry>& out, std::string& err) {
    if (n < 22) { err = "The bundled Android template is corrupt."; return false; }
    size_t eocd = 0;
    bool found = false;
    size_t back = n > 66000 ? n - 66000 : 0;
    for (size_t i = n - 22; i + 1 > back; --i) {
        if (Get32(apk + i) == 0x06054b50) { eocd = i; found = true; break; }
        if (i == 0) break;
    }
    if (!found) { err = "The bundled Android template is not a valid archive."; return false; }
    const uint16_t count = Get16(apk + eocd + 10);
    const uint32_t cdOfs = Get32(apk + eocd + 16);
    size_t p = cdOfs;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > n || Get32(apk + p) != 0x02014b50) { err = "Android template: bad directory."; return false; }
        const uint16_t method = Get16(apk + p + 10);
        const uint32_t csize = Get32(apk + p + 20);
        const uint16_t nameLen = Get16(apk + p + 28);
        const uint16_t extraLen = Get16(apk + p + 30);
        const uint16_t cmtLen = Get16(apk + p + 32);
        const uint32_t lofs = Get32(apk + p + 42);
        std::string name((const char*)(apk + p + 46), nameLen);
        p += 46 + nameLen + extraLen + cmtLen;
        // Signature files are re-made here; skip whatever the template happened to carry.
        if (name.rfind("META-INF/", 0) == 0) continue;
        if (name.empty() || name.back() == '/') continue;         // directory record
        if (method != 0) {
            err = "The bundled Android template has a compressed entry (" + name +
                  "); it must be built with build_apk.ps1 -Template.";
            return false;
        }
        if (lofs + 30 > n) { err = "Android template: bad entry offset."; return false; }
        const uint16_t lNameLen = Get16(apk + lofs + 26);
        const uint16_t lExtraLen = Get16(apk + lofs + 28);
        const size_t dataAt = lofs + 30 + lNameLen + lExtraLen;
        if (dataAt + csize > n) { err = "Android template: entry runs past the file."; return false; }
        TplEntry e; e.name = name; e.data = apk + dataAt; e.size = csize;
        out.push_back(e);
    }
    return !out.empty();
}

// ---------------------------------------------------------------- the asset walk
struct PakFile {
    std::wstring full;
    std::string rel;              // '/'-separated, as the app will recreate it
    uint64_t size = 0;
};

void WalkDir(const std::wstring& dir, const std::string& relPrefix, std::vector<PakFile>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        char nameA[MAX_PATH];
        int an = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nameA, sizeof nameA, nullptr, nullptr);
        if (an <= 0) continue;
        std::string rel = relPrefix.empty() ? std::string(nameA) : relPrefix + "/" + nameA;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            WalkDir(full, rel, out);
        } else {
            PakFile f;
            f.full = full;
            f.rel = rel;
            f.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            out.push_back(f);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------- v1 (JAR) signing
std::string ManifestSection(const std::string& name, const std::string& digestB64) {
    // 72-byte line wrapping, per the jar spec: a continuation begins with a single space.
    std::string head = "Name: " + name;
    std::string wrapped;
    size_t at = 0;
    while (head.size() - at > 72) {
        wrapped += head.substr(at, 72) + "\r\n ";
        at += 72;
    }
    wrapped += head.substr(at);
    return wrapped + "\r\nSHA-256-Digest: " + digestB64 + "\r\n\r\n";
}

bool Pkcs7Sign(const void* pfx, size_t pfxBytes, const wchar_t* pw,
               const std::vector<uint8_t>& data, std::vector<uint8_t>& out, std::string& err) {
    CRYPT_DATA_BLOB blob;
    blob.pbData = (BYTE*)const_cast<void*>(pfx);
    blob.cbData = (DWORD)pfxBytes;
    // ⚠ NOT PKCS12_NO_PERSIST_KEY. That imports the private key as an EPHEMERAL CNG key, and
    // CryptSignMessage (a legacy CAPI entry point) then cannot reach it — signing fails with
    // NTE_BAD_KEYSET while every other step reports success. Importing to a user key set gives
    // CryptSignMessage a key it can use; CRYPT_USER_KEYSET keeps it out of the machine store.
    HCERTSTORE store = PFXImportCertStore(&blob, pw, CRYPT_EXPORTABLE | CRYPT_USER_KEYSET);
    if (!store) {
        err = "The signing key could not be read (error " + std::to_string((unsigned)GetLastError()) + ").";
        return false;
    }
    PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                     0, CERT_FIND_ANY, nullptr, nullptr);
    if (!cert) { CertCloseStore(store, 0); err = "The signing certificate is missing."; return false; }

    CRYPT_SIGN_MESSAGE_PARA para = {};
    para.cbSize = sizeof para;
    para.dwMsgEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    para.pSigningCert = cert;
    para.HashAlgorithm.pszObjId = const_cast<char*>(szOID_NIST_sha256);
    para.cMsgCert = 1;
    para.rgpMsgCert = &cert;

    const BYTE* msg = data.data();
    DWORD msgLen = (DWORD)data.size();
    DWORD outLen = 0;
    BOOL ok = CryptSignMessage(&para, TRUE /*detached*/, 1, &msg, &msgLen, nullptr, &outLen);
    if (ok) {
        out.resize(outLen);
        ok = CryptSignMessage(&para, TRUE, 1, &msg, &msgLen, out.data(), &outLen);
        out.resize(outLen);
    }
    if (!ok) err = "The Android package could not be signed (error " +
                   std::to_string((unsigned)GetLastError()) + ")."; 
    // The import above put the key in the user's key set; take it back out. The PFX file is
    // the only copy that should outlive this call.
    DWORD kpiLen = 0;
    if (CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &kpiLen) && kpiLen) {
        std::vector<uint8_t> kpiBuf(kpiLen);
        if (CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, kpiBuf.data(), &kpiLen)) {
            const CRYPT_KEY_PROV_INFO* kpi = (const CRYPT_KEY_PROV_INFO*)kpiBuf.data();
            HCRYPTPROV scrub = 0;
            CryptAcquireContextW(&scrub, kpi->pwszContainerName, kpi->pwszProvName,
                                 kpi->dwProvType, CRYPT_DELETEKEYSET);
        }
    }
    CertFreeCertificateContext(cert);
    CertCloseStore(store, 0);
    return ok != FALSE;
}

// Make a fresh, uniquely named CAPI container. Used both when generating the identity and
// when signing from a stored key blob, which needs somewhere to import it to.
bool NewContainer(std::wstring& name, HCRYPTPROV& prov, DWORD& lastErr) {
    wchar_t buf[64];
    GUID g;
    CoCreateGuid(&g);
    swprintf_s(buf, L"tjwow-apk-%08lx%04hx%04hx", g.Data1, g.Data2, g.Data3);
    prov = 0;
    if (!CryptAcquireContextW(&prov, buf, kCsp, PROV_RSA_AES, CRYPT_NEWKEYSET)) {
        lastErr = GetLastError(); return false;
    }
    name = buf;
    return true;
}
void DeleteContainer(const std::wstring& name) {
    HCRYPTPROV p = 0;
    CryptAcquireContextW(&p, name.c_str(), kCsp, PROV_RSA_AES, CRYPT_DELETEKEYSET);
}

// Sign from the certificate plus a raw PRIVATEKEYBLOB -- the form used when the PFX export was
// refused. Same CryptSignMessage, same detached PKCS#7, same certificate; the only difference
// is where the key comes from. The key is imported into a SCRATCH container that exists only
// for this call and is deleted straight after, so nothing about the identity lives outside the
// two files on disk.
bool Pkcs7SignBlob(const SigningKey& key, const std::vector<uint8_t>& data,
                   std::vector<uint8_t>& out, std::string& err) {
    std::wstring cont;
    HCRYPTPROV prov = 0;
    DWORD e = 0;
    if (!NewContainer(cont, prov, e)) {
        err = "A scratch key container could not be created (error " +
              std::to_string((unsigned)e) + ")."; return false;
    }
    HCRYPTKEY hk = 0;
    if (!CryptImportKey(prov, key.keyBlob.data(), (DWORD)key.keyBlob.size(), 0, 0, &hk)) {
        e = GetLastError();
        CryptReleaseContext(prov, 0); DeleteContainer(cont);
        err = "The stored signing key could not be loaded (CryptImportKey, error " +
              std::to_string((unsigned)e) + ")."; return false;
    }
    CryptDestroyKey(hk);

    bool ok = false;
    PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                       key.cerDer.data(), (DWORD)key.cerDer.size());
    if (!cert) {
        err = "The signing certificate could not be read (error " +
              std::to_string((unsigned)GetLastError()) + ").";
    } else {
        CRYPT_KEY_PROV_INFO kpi = {};
        kpi.pwszContainerName = const_cast<LPWSTR>(cont.c_str());
        kpi.pwszProvName = const_cast<LPWSTR>(kCsp);
        kpi.dwProvType = PROV_RSA_AES;
        kpi.dwKeySpec = AT_SIGNATURE;
        if (!CertSetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi)) {
            err = "The signing key could not be attached (error " +
                  std::to_string((unsigned)GetLastError()) + ").";
        } else {
            CRYPT_SIGN_MESSAGE_PARA para = {};
            para.cbSize = sizeof para;
            para.dwMsgEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
            para.pSigningCert = cert;
            para.HashAlgorithm.pszObjId = const_cast<char*>(szOID_NIST_sha256);
            para.cMsgCert = 1;
            para.rgpMsgCert = &cert;
            const BYTE* msg = data.data();
            DWORD msgLen = (DWORD)data.size();
            DWORD outLen = 0;
            BOOL b = CryptSignMessage(&para, TRUE /*detached*/, 1, &msg, &msgLen, nullptr, &outLen);
            if (b) {
                out.resize(outLen);
                b = CryptSignMessage(&para, TRUE, 1, &msg, &msgLen, out.data(), &outLen);
                out.resize(outLen);
            }
            ok = (b != FALSE);
            if (!ok) err = "The Android package could not be signed (stored key, error " +
                           std::to_string((unsigned)GetLastError()) + ").";
        }
        CertFreeCertificateContext(cert);
    }
    CryptReleaseContext(prov, 0);
    DeleteContainer(cont);
    return ok;
}

bool Pkcs7SignKey(const SigningKey& key, const std::vector<uint8_t>& data,
                  std::vector<uint8_t>& out, std::string& err) {
    if (key.UsesPfx())
        return Pkcs7Sign(key.pfx.data(), key.pfx.size(), kPfxPassword, data, out, err);
    return Pkcs7SignBlob(key, data, out, err);
}


// ---------------------------------------------------------------- the player's signing key
//
// Generated once, then reused forever. Android identifies an app by its signing certificate,
// so reusing this key is what lets a LATER installer build an apk that updates the player's
// existing install instead of being refused with INSTALL_FAILED_UPDATE_INCOMPATIBLE.
//
// It is deliberately NOT shipped with the installer. A key embedded in a published .exe is a
// published key -- anyone can extract it and sign an apk Android would treat as this same app.
// Generating per player costs nothing and avoids that entirely.
bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (16 << 20)) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    CloseHandle(h);
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    CloseHandle(h);
    return ok;
}

// Export the store as a PFX. The LEGACY format (the default) encrypts the certificate bag
// with RC2 and the shrouded key bag with 3DES -- and RC2 is NOT a FIPS-approved algorithm, so
// on a machine with "System cryptography: Use FIPS compliant algorithms" enabled this call
// fails outright while every step before it succeeded. PBES2 (AES-256/SHA-256, Windows 8+) is
// approved, so a machine that refuses the legacy format is asked again in the modern one.
// Legacy is still TRIED FIRST on purpose: it is what every existing player's key was written
// in, and this file is the only thing that reads them back, so nothing already working changes
// format under it.
bool ExportPfx(HCERTSTORE mem, bool pbes2, std::vector<uint8_t>& pfx, DWORD& lastErr) {
    PKCS12_PBES2_EXPORT_PARAMS p2 = {};
    void* para = nullptr;
    DWORD flags = EXPORT_PRIVATE_KEYS;
    if (pbes2) {
        p2.dwSize = sizeof p2;
        p2.hNcryptDescriptor = nullptr;
        p2.pwszPbes2Alg = const_cast<LPWSTR>(PKCS12_PBES2_ALG_AES256_SHA256);
        para = &p2;
        flags |= PKCS12_EXPORT_PBES2_PARAMS;
    }
    CRYPT_DATA_BLOB out = {};
    SetLastError(0);
    if (!PFXExportCertStoreEx(mem, &out, kPfxPassword, para, flags) || out.cbData == 0) {
        lastErr = GetLastError(); return false;              // (0 here means it refused silently)
    }
    pfx.resize(out.cbData);
    out.pbData = pfx.data();
    SetLastError(0);
    if (!PFXExportCertStoreEx(mem, &out, kPfxPassword, para, flags) || out.cbData == 0) {
        lastErr = GetLastError(); pfx.clear(); return false;
    }
    pfx.resize(out.cbData);
    return true;
}

// ⚠ NAME THE STEP AND THE ERROR CODE. This used to collapse five distinct API failures into
// one message with no code, and the first field report of it ("A signing certificate could not
// be created", PC install fine) was therefore impossible to act on: nothing said whether the
// subject name, the certificate, the memory store or the PFX export was what refused, let
// alone why. Every failure now carries the call that failed and its GetLastError.
bool GenerateSigningKey(SigningKey& out, std::string& err) {
    // A named container is needed for CertCreateSelfSignCertificate to find the private key;
    // it is deleted again at the end, because the PFX bytes are the only copy we keep.
    wchar_t container[64];
    GUID g;
    CoCreateGuid(&g);
    swprintf_s(container, L"tjwow-apk-%08lx%04hx%04hx", g.Data1, g.Data2, g.Data3);

    HCRYPTPROV prov = 0;
    if (!CryptAcquireContextW(&prov, container, kCsp, PROV_RSA_AES, CRYPT_NEWKEYSET)) {
        err = "A signing key could not be created (CryptAcquireContext, error " +
              std::to_string((unsigned)GetLastError()) + ")."; return false;
    }
    HCRYPTKEY key = 0;
    if (!CryptGenKey(prov, AT_SIGNATURE, (2048 << 16) | CRYPT_EXPORTABLE, &key)) {
        DWORD e = GetLastError();
        CryptReleaseContext(prov, 0);
        CryptAcquireContextW(&prov, container, kCsp, PROV_RSA_AES, CRYPT_DELETEKEYSET);
        err = "A signing key could not be generated (CryptGenKey, error " +
              std::to_string((unsigned)e) + ")."; return false;
    }
    CryptDestroyKey(key);

    bool ok = false;
    const char* step = "CertStrToName";      // the last call attempted, for the message below
    DWORD lastErr = 0;
    BYTE nameBuf[512];
    DWORD nameLen = sizeof nameBuf;
    CERT_NAME_BLOB subject = { 0, nameBuf };
    if (!CertStrToNameW(X509_ASN_ENCODING,
                        L"CN=Tom and Jerry War of the Whiskers, O=Native port",
                        CERT_X500_NAME_STR, nullptr, nameBuf, &nameLen, nullptr)) {
        lastErr = GetLastError();
    } else {
        subject.cbData = nameLen;
        CRYPT_KEY_PROV_INFO kpi = {};
        kpi.pwszContainerName = container;
        kpi.pwszProvName = const_cast<LPWSTR>(kCsp);      // never "whatever the default is"
        kpi.dwProvType = PROV_RSA_AES;
        kpi.dwKeySpec = AT_SIGNATURE;
        CRYPT_ALGORITHM_IDENTIFIER alg = {};
        alg.pszObjId = const_cast<char*>(szOID_RSA_SHA256RSA);
        // Android wants a certificate that outlives the app; 30 years is the usual choice.
        SYSTEMTIME endSt;
        GetSystemTime(&endSt);
        endSt.wYear = (WORD)(endSt.wYear + 30);
        if (endSt.wMonth == 2 && endSt.wDay == 29) endSt.wDay = 28;   // leap-day safety
        step = "CertCreateSelfSignCertificate";
        PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(prov, &subject, 0, &kpi, &alg,
                                                            nullptr, &endSt, nullptr);
        if (!cert) {
            lastErr = GetLastError();
        } else {
            step = "CertOpenStore";
            HCERTSTORE mem = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
            if (!mem) {
                lastErr = GetLastError();
            } else if (!CertAddCertificateContextToStore(mem, cert, CERT_STORE_ADD_ALWAYS, nullptr)) {
                step = "CertAddCertificateContextToStore"; lastErr = GetLastError();
            } else {
                step = "PFXExportCertStoreEx";
                DWORD pbErr = 0;
                // ⚠ REPORT BOTH REFUSALS. The first version of this reported only the legacy
                // code and threw the PBES2 one away, which is how a field report came back
                // reading "error 0" and still could not be acted on. Two formats failing for
                // two different reasons is a different diagnosis from two failing for one.
                if (!ForceExportFailure()) {
                    ok = ExportPfx(mem, false, out.pfx, lastErr);
                    if (!ok) ok = ExportPfx(mem, true, out.pfx, pbErr);   // AES-256 (FIPS-safe)
                }
                DWORD lastErr2 = lastErr;            // the legacy refusal, kept for the note
                if (!ok) {
                    // The PKCS#12 packaging was refused; the identity itself is fine. Take the
                    // certificate and the private key straight out, as files. CryptExportKey
                    // touches none of the machinery that just said no.
                    step = "CryptExportKey";
                    HCRYPTKEY uk = 0;
                    DWORD blobLen = 0;
                    if (!CryptGetUserKey(prov, AT_SIGNATURE, &uk)) {
                        lastErr = GetLastError();
                    } else if (!CryptExportKey(uk, 0, PRIVATEKEYBLOB, 0, nullptr, &blobLen) ||
                               blobLen == 0) {
                        lastErr = GetLastError();
                    } else {
                        out.keyBlob.resize(blobLen);
                        if (!CryptExportKey(uk, 0, PRIVATEKEYBLOB, 0, out.keyBlob.data(), &blobLen)) {
                            lastErr = GetLastError();
                            out.keyBlob.clear();
                        } else {
                            out.keyBlob.resize(blobLen);
                            out.pfx.clear();
                            out.cerDer.assign(cert->pbCertEncoded,
                                              cert->pbCertEncoded + cert->cbCertEncoded);
                            ok = !out.cerDer.empty();
                            out.note = "PFX export refused (legacy error " +
                                       std::to_string((unsigned)lastErr2) + ", PBES2 error " +
                                       std::to_string((unsigned)pbErr) +
                                       "); the key was stored as a raw blob instead.";
                        }
                    }
                    if (uk) CryptDestroyKey(uk);
                }
            }
            if (mem) CertCloseStore(mem, 0);
            CertFreeCertificateContext(cert);
        }
    }
    CryptReleaseContext(prov, 0);
    // ALWAYS deleted now: both forms are files, so nothing needs the container to outlive this.
    CryptAcquireContextW(&prov, container, kCsp, PROV_RSA_AES, CRYPT_DELETEKEYSET);
    if (!ok)
        err = std::string("A signing certificate could not be created (") + step + ", error " +
              std::to_string((unsigned)lastErr) + ").";
    return ok;
}

// Load the player's key, creating it the first time. `keyFile`'s directory must exist.
// ⚠ THE PFX PATH IS CHECKED FIRST AND IS UNCHANGED. Every player whose key already works keeps
// working byte for byte; the container form below is only ever reached on a machine where the
// export failed, so it cannot regress anyone.
bool EnsureSigningKey(const std::wstring& keyFile, SigningKey& key, std::string& err) {
    const std::wstring cerFile = keyFile + L".cer";
    const std::wstring keyBlobFile = keyFile + L".key";
    const std::wstring cspFile = keyFile + L".csp";      // the retired container form

    if (ReadWholeFile(keyFile, key.pfx) && !key.pfx.empty()) return true;   // form 1, unchanged
    key.pfx.clear();

    // Form 2: the certificate beside its private key, both files.
    if (ReadWholeFile(cerFile, key.cerDer) && !key.cerDer.empty() &&
        ReadWholeFile(keyBlobFile, key.keyBlob) && !key.keyBlob.empty())
        return true;
    key.cerDer.clear(); key.keyBlob.clear();

    // ⚠ THE RETIRED FORM, AND WHY IT IS HANDLED RATHER THAN IGNORED. An earlier build stored
    // only the container NAME and left the key inside it. A container is managed state -- one
    // field report came back CRYPT_E_NO_KEY_PROPERTY because it had been removed -- and the
    // leftover pair then failed FOREVER, since nothing revalidated it. So: if the container is
    // still usable, the identity is rescued by exporting the key to a FILE right now, which
    // keeps the player's certificate and lets their phone keep updating in place. If it is
    // gone, the stale files are DELETED and a fresh identity generated below, because failing
    // forever is the one outcome that must not be possible.
    std::vector<uint8_t> cspRaw;
    if (ReadWholeFile(cerFile, key.cerDer) && !key.cerDer.empty() &&
        ReadWholeFile(cspFile, cspRaw) && !cspRaw.empty()) {
        std::string nameA((const char*)cspRaw.data(), cspRaw.size());
        while (!nameA.empty() && (nameA.back() == 10 || nameA.back() == 13 || nameA.back() == 0))
            nameA.pop_back();
        std::wstring cont(nameA.begin(), nameA.end());   // ASCII by construction
        HCRYPTPROV prov = 0;
        HCRYPTKEY uk = 0;
        DWORD blobLen = 0;
        bool rescued = false;
        if (!cont.empty() &&
            CryptAcquireContextW(&prov, cont.c_str(), kCsp, PROV_RSA_AES, 0) &&
            CryptGetUserKey(prov, AT_SIGNATURE, &uk) &&
            CryptExportKey(uk, 0, PRIVATEKEYBLOB, 0, nullptr, &blobLen) && blobLen) {
            key.keyBlob.resize(blobLen);
            if (CryptExportKey(uk, 0, PRIVATEKEYBLOB, 0, key.keyBlob.data(), &blobLen)) {
                key.keyBlob.resize(blobLen);
                rescued = WriteWholeFile(keyBlobFile, key.keyBlob);
            } else key.keyBlob.clear();
        }
        if (uk) CryptDestroyKey(uk);
        if (prov) CryptReleaseContext(prov, 0);
        DeleteFileW(cspFile.c_str());          // retired either way
        if (rescued) return true;              // same certificate, now on a durable footing
        key.cerDer.clear(); key.keyBlob.clear();
        DeleteFileW(cerFile.c_str());          // the key is gone; the certificate is useless
    }
    key.cerDer.clear(); key.keyBlob.clear();

    if (!GenerateSigningKey(key, err)) return false;
    // Persisting is best-effort: an apk that cannot update a previous one still beats no apk.
    if (key.UsesPfx()) {
        WriteWholeFile(keyFile, key.pfx);
    } else {
        WriteWholeFile(cerFile, key.cerDer);
        WriteWholeFile(keyBlobFile, key.keyBlob);
        // The reason lands on disk too. This path is silent by design -- the player gets a
        // working apk and no scary message -- so without this nobody would ever learn WHY the
        // export was refused on that machine, which is the one thing we still want to know.
        if (!key.note.empty())
            WriteWholeFile(keyFile + L".txt",
                           std::vector<uint8_t>(key.note.begin(), key.note.end()));
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------
bool BuildAndroidApk(const void* templateApk, size_t templateBytes,
                     const std::wstring& keyFile,
                     const std::wstring& assetRoot,
                     const wchar_t* const* items, int itemCount,
                     const std::wstring& outApk,
                     ApkProgressFn cb, void* ctx, std::string& err) {
    std::vector<TplEntry> tpl;
    if (!ReadTemplate((const uint8_t*)templateApk, templateBytes, tpl, err)) return false;

    // Do this BEFORE writing 223 MB: a key problem should fail in a second, not a minute.
    SigningKey signKey;
    if (!EnsureSigningKey(keyFile, signKey, err)) return false;

    // What goes in the pack.
    std::vector<PakFile> files;
    for (int i = 0; i < itemCount; ++i) {
        std::wstring full = assetRoot + L"\\" + items[i];
        DWORD at = GetFileAttributesW(full.c_str());
        if (at == INVALID_FILE_ATTRIBUTES) continue;
        char nameA[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, items[i], -1, nameA, sizeof nameA, nullptr, nullptr);
        if (at & FILE_ATTRIBUTE_DIRECTORY) {
            WalkDir(full, nameA, files);
        } else {
            PakFile f;
            f.full = full;
            f.rel = nameA;
            WIN32_FILE_ATTRIBUTE_DATA d;
            if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &d))
                f.size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
            files.push_back(f);
        }
    }
    if (files.empty()) { err = "No game files were found to pack."; return false; }

    // The pack's index, and therefore its exact size, are known before a byte is written.
    std::vector<uint8_t> index;
    index.push_back('T'); index.push_back('J'); index.push_back('P'); index.push_back('K');
    Put32(index, 1);
    Put32(index, (uint32_t)files.size());
    uint64_t payload = 0;
    for (const PakFile& f : files) {
        Put16(index, (uint16_t)f.rel.size());
        index.insert(index.end(), f.rel.begin(), f.rel.end());
        for (int i = 0; i < 8; ++i) index.push_back((uint8_t)(f.size >> (i * 8)));
        payload += f.size;
    }
    const uint64_t pakSize = index.size() + payload;
    if (pakSize > 0xFFFFFFFFull) { err = "The game data is too large for one package."; return false; }

    OutFile out;
    if (!out.Open(outApk)) { err = "The Android package could not be created."; return false; }

    std::vector<OutEntry> dir;
    // name -> base64 SHA-256 of the entry's (uncompressed) bytes, for MANIFEST.MF
    std::vector<std::pair<std::string, std::string>> digests;

    // 1. the template's own entries, byte for byte
    for (const TplEntry& e : tpl) {
        if (!AddEntry(out, dir, e.name, e.data, e.size)) { err = "Writing the Android package failed."; return false; }
        std::vector<uint8_t> h;
        if (!Sha256Of(e.data, e.size, h)) { err = "Hashing the Android package failed."; return false; }
        digests.push_back({ e.name, Base64(h) });
    }

    // 2. assets/game.pak — streamed, so 223 MB never sits in memory. The local header goes
    //    down with a placeholder CRC and is patched once the bytes have been read.
    const std::string pakName = "assets/game.pak";
    const uint64_t pakLocalOfs = out.pos;
    {
        std::vector<uint8_t> hdr;
        LocalHeader(hdr, pakName, 0, pakSize);
        if (!out.Write(hdr)) { err = "Writing the Android package failed."; return false; }
    }
    Crc32 pakCrc;
    Sha256 pakSha;
    if (!pakSha.Begin()) { err = "Hashing the Android package failed."; return false; }
    pakCrc.Update(index.data(), index.size());
    pakSha.Update(index.data(), index.size());
    if (!out.Write(index)) { err = "Writing the Android package failed."; return false; }

    std::vector<uint8_t> buf(1u << 20);
    uint64_t done = 0;
    for (const PakFile& f : files) {
        HANDLE in = CreateFileW(f.full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (in == INVALID_HANDLE_VALUE) { err = "A game file could not be read while packing."; return false; }
        uint64_t left = f.size;
        while (left) {
            DWORD want = (DWORD)(left < buf.size() ? left : buf.size());
            DWORD got = 0;
            if (!ReadFile(in, buf.data(), want, &got, nullptr) || got == 0) {
                CloseHandle(in); err = "A game file ended early while packing."; return false;
            }
            pakCrc.Update(buf.data(), got);
            pakSha.Update(buf.data(), got);
            if (!out.Write(buf.data(), got)) { CloseHandle(in); err = "Writing the Android package failed."; return false; }
            left -= got;
            done += got;
        }
        CloseHandle(in);
        if (cb && !cb(ctx, L"Packing the game into the Android app\x2026", done, payload)) {
            err = "cancelled"; return false;
        }
    }
    {
        const uint32_t crc = pakCrc.Final();
        if (!out.PatchAt(pakLocalOfs + 14, &crc, 4)) { err = "Writing the Android package failed."; return false; }
        OutEntry e; e.name = pakName; e.crc = crc; e.size = pakSize; e.localOfs = pakLocalOfs;
        dir.push_back(e);
        std::vector<uint8_t> h;
        if (!pakSha.Final(h)) { err = "Hashing the Android package failed."; return false; }
        digests.push_back({ pakName, Base64(h) });
    }

    // 3. the v1 signature, over everything above
    if (cb) cb(ctx, L"Signing the Android app\x2026", payload, payload);
    std::string manifest = "Manifest-Version: 1.0\r\nCreated-By: Tom & Jerry WOW installer\r\n\r\n";
    const size_t mainLen = manifest.size();
    for (const auto& d : digests) manifest += ManifestSection(d.first, d.second);

    std::vector<uint8_t> h;
    if (!Sha256Of(manifest.data(), manifest.size(), h)) { err = "Signing failed."; return false; }
    std::string sf = "Signature-Version: 1.0\r\nSHA-256-Digest-Manifest: " + Base64(h) + "\r\n";
    if (!Sha256Of(manifest.data(), mainLen, h)) { err = "Signing failed."; return false; }
    sf += "SHA-256-Digest-Manifest-Main-Attributes: " + Base64(h) + "\r\nCreated-By: Tom & Jerry WOW installer\r\n\r\n";
    {   // each .SF section digests that entry's MANIFEST SECTION TEXT, not the entry itself
        size_t at = mainLen;
        for (const auto& d : digests) {
            std::string sec = ManifestSection(d.first, d.second);
            if (!Sha256Of(manifest.data() + at, sec.size(), h)) { err = "Signing failed."; return false; }
            sf += ManifestSection(d.first, Base64(h));
            at += sec.size();
        }
    }
    std::vector<uint8_t> sfBytes(sf.begin(), sf.end());
    std::vector<uint8_t> p7;
    if (!Pkcs7SignKey(signKey, sfBytes, p7, err)) return false;

    if (!AddEntry(out, dir, "META-INF/MANIFEST.MF", manifest.data(), manifest.size()) ||
        !AddEntry(out, dir, "META-INF/TJSIGN.SF", sf.data(), sf.size()) ||
        !AddEntry(out, dir, "META-INF/TJSIGN.RSA", p7.data(), p7.size())) {
        err = "Writing the Android package failed."; return false;
    }

    if (!WriteCentralDirectory(out, dir)) { err = "Writing the Android package failed."; return false; }
    out.Close();
    return true;
}

} // namespace tj::setup
