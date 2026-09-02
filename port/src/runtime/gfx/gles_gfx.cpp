// GLES3 GRAPHICS BACKEND — the Android platform layer's tj::gfx::Device (runtime/gfx/d3d8.h).
//
// A faithful port of the D3D11 backend (d3d8.cpp): the bridge (d3d8_bridge.cpp) does all the
// semantic CPU work (TransformPoint, TriangulateToList, skinning, the shadowed g_vsc
// constants) and hands us plain textured triangle lists — so this layer is small and needs NO
// Xbox shader translation. It reproduces d3d8.cpp's THREE pipelines (colored / textured /
// shiny 4-texture combiner), the texture table with a free-list + recycle pool + full mips,
// FBO render-to-texture, the depth/blend/sampler states, and the D3DCOLOR (0xAARRGGBB → BGRA
// bytes) convention. It OWNS EGL: Create() takes the ANativeWindow (as HWND) and builds the
// display/context/surface; Present() swaps. Render resolution = the surface's native size
// (ANDROID_PLAN §2.7: Android has no video-resolution settings).
//
// ⚠ FIRST PASS — compiled against the NDK but not yet run on a device. The D3D→GLES
// conventions handled here (documented at each site so on-device tuning is a one-liner):
//   * depth range: D3D clip z∈[0,w] → GL z∈[-w,w]  (VS: z = 2z - w)
//   * matrix: game hands a ROW-major WVP; uploaded transpose=GL_TRUE + `v * M` in the VS
//     reproduces D3D's mul(rowVec, M) exactly.
//   * NDC is Y-up in BOTH APIs → geometry needs no Y flip.
//   * texture memory: GL's first data row is v=0=BOTTOM, D3D's is v=0=TOP → asset textures
//     are uploaded row-reversed so the game's D3D-authored UVs sample correctly.
//   * D3DCOLOR byte order [B,G,R,A]: vertex colour reordered in-shader (.bgra); asset-texture
//     R/B swapped via GL_TEXTURE_SWIZZLE. Render/capture textures keep natural RGBA.
#include "runtime/gfx/d3d8.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>          // GL_TEXTURE_MAX_ANISOTROPY_EXT (if present)
#include <android/native_window.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <unordered_map>

namespace tj::gfx {
namespace {

// --- Shaders (GLSL ES 3.00). One per D3D11 pipeline in d3d8.cpp. ------------------------
// The `v * gWVP` (row-vector × matrix) with a transpose=GL_TRUE upload equals D3D's
// mul(float4(pos,1), gWVP) on a row_major matrix. `p.z = 2p.z - p.w` maps D3D depth to GL.
const char* kColorVS = R"(#version 300 es
uniform mat4 gWVP;
uniform int gFlip;
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aCol;
out vec4 vCol;
void main(){ vec4 p = vec4(aPos,1.0) * gWVP; p.z = 2.0*p.z - p.w; if (gFlip != 0) p.y = -p.y; gl_Position = p; vCol = aCol.bgra; })";
const char* kColorFS = R"(#version 300 es
precision highp float;
in vec4 vCol; out vec4 oCol;
void main(){ oCol = vCol; })";

const char* kTexVS = R"(#version 300 es
uniform mat4 gWVP;
uniform int gFlip;
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aCol;
out vec2 vUV; out vec4 vCol;
void main(){ vec4 p = vec4(aPos,1.0) * gWVP; p.z = 2.0*p.z - p.w; if (gFlip != 0) p.y = -p.y; gl_Position = p; vUV = aUV; vCol = aCol.bgra; })";
const char* kTexFS = R"(#version 300 es
precision highp float;
uniform sampler2D gTex;
in vec2 vUV; in vec4 vCol; out vec4 oCol;
void main(){ oCol = texture(gTex, vUV) * vCol; })";

// Shiny 4-stage combiner, the exact Xbox formula (d3d8.cpp kShinyShaderHLSL):
//   rgb = t0(uv0)*vcol + t2(uv1)*t1(uv0) + t3(uv1)*t1(uv0)*0.5 ,  a = t0.a*vcol.a
const char* kShinyVS = R"(#version 300 es
uniform mat4 gWVP;
uniform int gFlip;
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV0;
layout(location=2) in vec2 aUV1;
layout(location=3) in vec4 aCol;
out vec2 vUV0; out vec2 vUV1; out vec4 vCol;
void main(){ vec4 p = vec4(aPos,1.0) * gWVP; p.z = 2.0*p.z - p.w; if (gFlip != 0) p.y = -p.y; gl_Position = p; vUV0=aUV0; vUV1=aUV1; vCol=aCol.bgra; })";
const char* kShinyFS = R"(#version 300 es
precision highp float;
uniform sampler2D gT0, gT1, gT2, gT3;
in vec2 vUV0; in vec2 vUV1; in vec4 vCol; out vec4 oCol;
void main(){
    vec4 base = texture(gT0, vUV0) * vCol;
    vec3 mask = texture(gT1, vUV0).rgb;
    vec3 sheen = texture(gT2, vUV1).rgb * mask + texture(gT3, vUV1).rgb * mask * 0.5;
    oCol = vec4(base.rgb + sheen, base.a);
})";

GLuint Compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, nullptr, log);
               printf("[gles] shader compile failed: %s\n", log); glDeleteShader(s); return 0; }
    return s;
}
GLuint Link(const char* vs, const char* fs) {
    GLuint v = Compile(GL_VERTEX_SHADER, vs), f = Compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p, sizeof log, nullptr, log);
               printf("[gles] program link failed: %s\n", log); glDeleteProgram(p); return 0; }
    return p;
}

// The app sets the real display size once (no in-game resolution menu on Android).
int g_dispW = 1280, g_dispH = 720;
// The game's render size (16:9-clamped by the app). When set, backbuffer rendering is
// confined to a centered present rect of this size — pillarboxed on wider panels.
int g_gameW = 0, g_gameH = 0;

// --- DRAW-PATH DIAGNOSTICS (session 31) -------------------------------------------------
// The measured display cost is ~75-84 us PER DRAW at ~800 draws/frame, and three suspects
// are already dead: call count (7,300 -> 1,500 GL calls/frame moved nothing), fill (half
// resolution moved 84 -> 70-81 us) and textures (0.04 ms of a 67.9 ms replay). What is left
// inside a draw is the two streaming-buffer writes. This is the instrument that names the
// call, plus the "what if we simply did not do it" A/B that no reasoning can argue with.
//
// TJ_COMP_DIAG=<mask> in the flags file; 0 (absent) in every shipping run. The skip bits
// DELIBERATELY draw the wrong picture — they are measurement legs, never a shipping path.
//   bit 0 (1) — do not issue glDraw*            (uploads + state, no draw)
//   bit 1 (2) — do not upload vertices/indices  (draw from a zero-filled resident buffer)
//   bit 2 (4) — time each draw in four phases   (upload VB / upload IB / state / draw)
uint32_t g_diag = 0;
uint64_t g_diagNs[4] = {};      // vb, ib, state, draw
uint64_t g_diagDraws = 0;
bool     g_diagBufReady = false;

inline uint64_t DiagNowNs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
// Four-phase stopwatch; entirely dead (one predictable branch) unless bit 2 is set.
struct DiagPhase {
    uint64_t t; bool on;
    DiagPhase() : t(0), on((g_diag & 4u) != 0) { if (on) t = DiagNowNs(); }
    void mark(int i) { if (!on) return; uint64_t n = DiagNowNs(); g_diagNs[i] += n - t; t = n; }
};
// bit 1 needs the buffers to stay big enough that indices can never address past them, and
// ZERO-filled so every index reads vertex 0 (degenerate triangles, no fill) rather than
// undefined memory. One 10 MB upload at the first draw, then never touched again.
void DiagEnsureResidentBuffers(GLuint vbo, GLuint ibo) {
    if (g_diagBufReady) return;
    g_diagBufReady = true;
    const size_t vbBytes = 8u << 20, ibBytes = 2u << 20;
    void* zeros = calloc(1, vbBytes);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vbBytes, zeros, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ibBytes, zeros, GL_STREAM_DRAW);
    free(zeros);
    printf("[gles] TJ_COMP_DIAG=%u: resident %zu/%zu KB buffers, per-draw upload OFF\n",
           g_diag, vbBytes >> 10, ibBytes >> 10);
}

} // namespace

void SetAndroidDisplaySize(int w, int h) { if (w > 0 && h > 0) { g_dispW = w; g_dispH = h; } }
void GlesSetGameSize(int w, int h) { if (w > 0 && h > 0) { g_gameW = w; g_gameH = h; } }

// Draw-path diagnostics seam (the app reads TJ_COMP_DIAG from the flags file and sets it
// before the device is created). Drain returns ns totals for [vb, ib, state, draw] and the
// draw count since the previous drain, then zeroes them.
void GlesSetDrawDiag(uint32_t mask) { g_diag = mask; }
uint64_t GlesDrawDiagDrain(uint64_t out[4]) {
    for (int i = 0; i < 4; ++i) { out[i] = g_diagNs[i]; g_diagNs[i] = 0; }
    uint64_t n = g_diagDraws; g_diagDraws = 0; return n;
}

int EnumDisplayModes(DisplayMode* out, int maxOut) {
    if (out && maxOut >= 1) out[0] = { g_dispW, g_dispH, 60 };
    return maxOut >= 1 ? 1 : 0;
}
DisplayMode DesktopMode() { return { g_dispW, g_dispH, 60 }; }
int MaxMsaaSampleCount() { return 1; }   // default framebuffer only; no MSAA in this pass

struct Device::Impl {
    // EGL
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLConfig  cfg = nullptr;          // kept for window-surface recreation (app lifecycle)
    bool vsync = true;
    int  w = 0, h = 0;
    // Programs + uniform locations
    struct Prog { GLuint id = 0; GLint uWVP = -1; GLint uFlip = -1; } color, tex, shiny;
    GLint shinyT[4] = { -1, -1, -1, -1 };
    // gFlip: 1 while rendering into an FBO (D3D top-origin sampling vs GL bottom-up FBOs).
    int flip = 0;
    // Present rect: the game's 16:9 image centered in the (possibly wider) surface —
    // pillarboxed. Zero prW = disabled (viewport covers the surface, Windows-free path).
    int prX = 0, prY = 0, prW = 0, prH = 0;
    void BackbufferViewport() {
        if (prW > 0) glViewport(prX, prY, prW, prH);
        else glViewport(0, 0, w, h);
    }
    // Dynamic geometry (per-draw orphaned STREAM buffers — the d3d8.cpp ring is a later
    // perf optimization; correctness first).
    GLuint vao = 0, vbo = 0, ibo = 0;
    // Sampler objects: [uClamp][vClamp]
    GLuint samp[2][2] = {};
    GLuint curSamp = 0;
    // Texture table (mirrors d3d8.cpp: parallel arrays, free-list, recycle pool).
    struct Tex { GLuint id = 0; GLuint fbo = 0, depthRb = 0; int w = 0, h = 0; bool isRT = false; };
    std::vector<Tex>          texs;
    std::vector<TextureHandle> texFree;
    std::unordered_map<uint64_t, std::vector<TextureHandle>> texPool;
    int liveTextures = 0, pooledTextures = 0;
    int activeRT = -1;
    GLuint whiteTex = 0;         // 1x1 white default (untextured + stale-handle fallback)
    TextureHandle curTex = kNoTexture;
    float wvp[16];

    // Current render state (applied lazily like the D3D11 backend's state filter).
    void ApplyWVP(const Prog& pr) {
        glUseProgram(pr.id);
        glUniformMatrix4fv(pr.uWVP, 1, GL_TRUE, wvp);   // GL_TRUE: row-major → D3D mul(v,M)
        glUniform1i(pr.uFlip, flip);
    }
    GLuint TexId(TextureHandle t) {
        return (t >= 0 && t < (int)texs.size() && texs[t].id) ? texs[t].id : whiteTex;
    }
    TextureHandle AllocSlot(GLuint id, int tw, int th, bool isRT) {
        Tex slot; slot.id = id; slot.w = tw; slot.h = th; slot.isRT = isRT;
        if (!texFree.empty()) { TextureHandle h = texFree.back(); texFree.pop_back(); texs[h] = slot; return h; }
        texs.push_back(slot); return (TextureHandle)(texs.size() - 1);
    }
};

// The one live device. The batched-upload seam below is a set of free functions (the
// Android compositor's private path -- d3d8.h stays the shared contract with the D3D11
// backend), so it needs the live Impl by another route than `this`. Device::Impl is private:
// namespace scope cannot name it, but the friend functions in d3d8.h can, so it is parked
// here as void* and the type is recovered inside each of them.
static void* g_implV = nullptr;

// --- D3DCOLOR asset texture upload: BGRA→RGBA swizzle + row-flip (D3D top-origin) --------
static GLuint UploadAssetTexture(const uint32_t* rgba, int w, int h, bool flip) {
    GLuint id = 0; glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    // Row-flip so the game's D3D-authored UVs (v=0 = top) sample correctly under GL's
    // v=0 = bottom convention.
    const uint32_t* src = rgba;
    std::vector<uint32_t> flipped;
    if (flip && h > 1) {
        flipped.resize((size_t)w * h);
        for (int y = 0; y < h; ++y)
            memcpy(&flipped[(size_t)(h - 1 - y) * w], &rgba[(size_t)y * w], (size_t)w * 4);
        src = flipped.data();
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
    // D3DCOLOR bytes are [B,G,R,A]; swap R↔B at sample time so the shader gets true RGBA.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glGenerateMipmap(GL_TEXTURE_2D);
    return id;
}
static void ReuploadAssetTexture(GLuint id, const uint32_t* rgba, int w, int h, bool flip, bool genMips) {
    glBindTexture(GL_TEXTURE_2D, id);
    const uint32_t* src = rgba;
    std::vector<uint32_t> flipped;
    if (flip && h > 1) {
        flipped.resize((size_t)w * h);
        for (int y = 0; y < h; ++y)
            memcpy(&flipped[(size_t)(h - 1 - y) * w], &rgba[(size_t)y * w], (size_t)w * 4);
        src = flipped.data();
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, src);
    if (genMips) glGenerateMipmap(GL_TEXTURE_2D);
}

// Surface-handoff mirror (app lifecycle): pointers into the app's single Impl, captured in
// Create (member context — Impl is private, so free functions can't name it). See the
// GlesRelease/AttachWindowSurface functions at the bottom of this file.
static struct {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLConfig  cfg = nullptr;
    EGLSurface* surf = nullptr;
    bool vsync = true;
    int* w = nullptr; int* h = nullptr;
    int* prX = nullptr; int* prY = nullptr; int* prW = nullptr; int* prH = nullptr;
} g_appEgl;

bool Device::Create(HWND hwnd, const PresentParams& pp) {
    // The app retries Create on transient failures — every failure path MUST tear down the
    // half-built Impl (and its EGL objects) or the retry loop leaks a context per attempt.
    if (p_) Shutdown();
    p_ = new Impl();
    g_implV = p_;
    p_->vsync = pp.vsync;
    memset(p_->wvp, 0, sizeof p_->wvp);
    p_->wvp[0] = p_->wvp[5] = p_->wvp[10] = p_->wvp[15] = 1.0f;

    ANativeWindow* win = reinterpret_cast<ANativeWindow*>(hwnd);
    p_->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (p_->dpy == EGL_NO_DISPLAY) { printf("[gles] no EGL display\n"); Shutdown(); return false; }
    if (!eglInitialize(p_->dpy, nullptr, nullptr)) { printf("[gles] eglInitialize failed\n"); Shutdown(); return false; }
    const EGLint cfgAttr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint numCfg = 0;
    if (!eglChooseConfig(p_->dpy, cfgAttr, &cfg, 1, &numCfg) || numCfg < 1) {
        printf("[gles] eglChooseConfig failed\n"); Shutdown(); return false; }
    p_->cfg = cfg;
    if (win) {
        EGLint fmt = 0; eglGetConfigAttrib(p_->dpy, cfg, EGL_NATIVE_VISUAL_ID, &fmt);
        ANativeWindow_setBuffersGeometry(win, 0, 0, fmt);
    }
    const EGLint ctxAttr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    p_->ctx = eglCreateContext(p_->dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (p_->ctx == EGL_NO_CONTEXT) { printf("[gles] eglCreateContext failed\n"); Shutdown(); return false; }
    p_->surf = win ? eglCreateWindowSurface(p_->dpy, cfg, win, nullptr) : EGL_NO_SURFACE;
    if (p_->surf == EGL_NO_SURFACE) { printf("[gles] eglCreateWindowSurface failed\n"); Shutdown(); return false; }
    if (!eglMakeCurrent(p_->dpy, p_->surf, p_->surf, p_->ctx)) {
        printf("[gles] eglMakeCurrent failed\n"); Shutdown(); return false; }
    eglSwapInterval(p_->dpy, p_->vsync ? 1 : 0);
    EGLint sw = 0, sh = 0;
    eglQuerySurface(p_->dpy, p_->surf, EGL_WIDTH, &sw);
    eglQuerySurface(p_->dpy, p_->surf, EGL_HEIGHT, &sh);
    p_->w = sw > 0 ? sw : pp.backWidth;
    p_->h = sh > 0 ? sh : pp.backHeight;
    printf("[gles] GLES3 device %dx%d (%s)\n", p_->w, p_->h, glGetString(GL_RENDERER));

    // Programs
    p_->color.id = Link(kColorVS, kColorFS);
    p_->tex.id   = Link(kTexVS, kTexFS);
    p_->shiny.id = Link(kShinyVS, kShinyFS);
    if (!p_->color.id || !p_->tex.id || !p_->shiny.id) { Shutdown(); return false; }
    p_->color.uWVP = glGetUniformLocation(p_->color.id, "gWVP");
    p_->tex.uWVP   = glGetUniformLocation(p_->tex.id, "gWVP");
    p_->shiny.uWVP = glGetUniformLocation(p_->shiny.id, "gWVP");
    p_->color.uFlip = glGetUniformLocation(p_->color.id, "gFlip");
    p_->tex.uFlip   = glGetUniformLocation(p_->tex.id, "gFlip");
    p_->shiny.uFlip = glGetUniformLocation(p_->shiny.id, "gFlip");
    glUseProgram(p_->tex.id);
    glUniform1i(glGetUniformLocation(p_->tex.id, "gTex"), 0);
    glUseProgram(p_->shiny.id);
    const char* sn[4] = { "gT0", "gT1", "gT2", "gT3" };
    for (int i = 0; i < 4; ++i) { p_->shinyT[i] = glGetUniformLocation(p_->shiny.id, sn[i]);
                                  glUniform1i(p_->shinyT[i], i); }

    glGenVertexArrays(1, &p_->vao);
    glGenBuffers(1, &p_->vbo);
    glGenBuffers(1, &p_->ibo);

    // Samplers: [uClamp][vClamp]. Trilinear + anisotropic (if the EXT is present, like the
    // Xbox init). WRAP vs CLAMP_TO_EDGE per axis.
    float maxAniso = 1.0f;
    { const char* ext = (const char*)glGetString(GL_EXTENSIONS);
      if (ext && strstr(ext, "GL_EXT_texture_filter_anisotropic"))
          glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso); }
    for (int u = 0; u < 2; ++u)
        for (int v = 0; v < 2; ++v) {
            GLuint s; glGenSamplers(1, &s);
            glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glSamplerParameteri(s, GL_TEXTURE_WRAP_S, u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glSamplerParameteri(s, GL_TEXTURE_WRAP_T, v ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            if (maxAniso > 1.0f)
                glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAniso < 4.0f ? maxAniso : 4.0f);
            p_->samp[u][v] = s;
        }
    p_->curSamp = p_->samp[0][0];

    glDisable(GL_CULL_FACE);           // d3d8.cpp: CULL_NONE (engine/UI need it off)
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    if (g_gameW > 0) {                 // center the game's 16:9 image (pillarbox)
        p_->prW = g_gameW; p_->prH = g_gameH;
        p_->prX = (p_->w - g_gameW) / 2; p_->prY = (p_->h - g_gameH) / 2;
        if (p_->prX < 0) p_->prX = 0;
        if (p_->prY < 0) p_->prY = 0;
    }
    p_->BackbufferViewport();

    // 1x1 white default texture (untextured draws + stale-handle fallback → white, not black).
    uint32_t white = 0xffffffffu;
    p_->whiteTex = UploadAssetTexture(&white, 1, 1, false);

    // Capture the surface-handoff mirror (see GlesRelease/AttachWindowSurface).
    g_appEgl.dpy = p_->dpy; g_appEgl.ctx = p_->ctx; g_appEgl.cfg = p_->cfg;
    g_appEgl.surf = &p_->surf; g_appEgl.vsync = p_->vsync;
    g_appEgl.w = &p_->w; g_appEgl.h = &p_->h;
    g_appEgl.prX = &p_->prX; g_appEgl.prY = &p_->prY;
    g_appEgl.prW = &p_->prW; g_appEgl.prH = &p_->prH;
    return true;
}

void Device::Shutdown() {
    if (!p_) return;
    if (p_->dpy != EGL_NO_DISPLAY) {
        for (auto& t : p_->texs) { if (t.id) glDeleteTextures(1, &t.id);
                                   if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
                                   if (t.depthRb) glDeleteRenderbuffers(1, &t.depthRb); }
        if (p_->whiteTex) glDeleteTextures(1, &p_->whiteTex);
        for (int u = 0; u < 2; ++u) for (int v = 0; v < 2; ++v) if (p_->samp[u][v]) glDeleteSamplers(1, &p_->samp[u][v]);
        if (p_->vbo) glDeleteBuffers(1, &p_->vbo);
        if (p_->ibo) glDeleteBuffers(1, &p_->ibo);
        if (p_->vao) glDeleteVertexArrays(1, &p_->vao);
        if (p_->color.id) glDeleteProgram(p_->color.id);
        if (p_->tex.id) glDeleteProgram(p_->tex.id);
        if (p_->shiny.id) glDeleteProgram(p_->shiny.id);
        eglMakeCurrent(p_->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (p_->surf != EGL_NO_SURFACE) eglDestroySurface(p_->dpy, p_->surf);
        if (p_->ctx != EGL_NO_CONTEXT) eglDestroyContext(p_->dpy, p_->ctx);
        eglTerminate(p_->dpy);
    }
    delete p_; p_ = nullptr; g_implV = nullptr;
}

bool Device::Valid() const { return p_ && p_->ctx != EGL_NO_CONTEXT; }
ID3D11Device* Device::D3D11() const { return nullptr; }

void Device::Clear(uint32_t flags, uint32_t argb, float z, uint8_t stencil) {
    if (!p_) return;
    const bool boxed = p_->activeRT < 0 && p_->prW > 0;
    if (boxed && (flags & CLEAR_TARGET)) {
        glClearColor(0, 0, 0, 1);                    // pillars: always black
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(p_->prX, p_->prY, p_->prW, p_->prH);
    }
    GLbitfield mask = 0;
    if (flags & CLEAR_TARGET) {
        glClearColor(((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f,
                     (argb & 0xff) / 255.0f, ((argb >> 24) & 0xff) / 255.0f);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & CLEAR_ZBUFFER) { glClearDepthf(z); glDepthMask(GL_TRUE); mask |= GL_DEPTH_BUFFER_BIT; }
    if (flags & CLEAR_STENCIL) { glClearStencil(stencil); mask |= GL_STENCIL_BUFFER_BIT; }
    if (mask) glClear(mask);
    if (boxed && (flags & CLEAR_TARGET)) glDisable(GL_SCISSOR_TEST);
}

void Device::BeginScene() {}
void Device::EndScene() {}
void Device::Present() {
    if (p_ && p_->dpy != EGL_NO_DISPLAY) eglSwapBuffers(p_->dpy, p_->surf);
}

void Device::SetTransform(const float m[16]) { if (p_) memcpy(p_->wvp, m, sizeof p_->wvp); }

// --- Draw paths -------------------------------------------------------------------------
// THE PER-DRAW UPLOAD IS WHAT THE DISPLAY LEG COSTS -- MEASURED, not guessed (session 31).
// In-match at 253 draws/frame, with the four-phase timers below: the vertex glBufferData took
// 5.44 ms of the 6.99 ms spent in draws, the index one 0.91 ms, all state and attribute setup
// 0.16 ms, and the actual glDrawElements 0.44 ms. That is ~21.5 us of driver time per vertex
// upload at ~2 uploads per draw. Call count was already ruled out (7,300 -> 1,500 GL calls per
// frame moved nothing), fill was ruled out (half resolution moved 84 -> 70-81 us/draw), and
// textures own 0.04 ms of a 67.9 ms replay.
//
// So the geometry of every draw is now placed in the stream buffers ONCE PER FRAME by the
// compositor (GlesStreamUpload) and each draw names its slice by BYTE OFFSET. The pointer-
// taking entry points below stay exactly as they were -- they upload, then run the same
// offset-based body -- so the legacy path (and the d3d8.h contract shared with the Windows
// D3D11 backend) is untouched, and remains both the A/B leg and the fallback.

// Upload one frame of vertex + index bytes in a single re-specification each. Adreno wants
// exactly this shape: orphan (glBufferData WITH data) rather than a write into a buffer the
// GPU may still be reading.
bool GlesStreamUpload(const void* vb, unsigned long vbBytes, const void* ib, unsigned long ibBytes) {
    Device::Impl* p = (Device::Impl*)g_implV;
    if (!p) return false;
    DiagPhase dp;
    if (g_diag & 2u) { DiagEnsureResidentBuffers(p->vbo, p->ibo); return true; }
    if (vbBytes) {
        glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vbBytes, vb, GL_STREAM_DRAW);
    }
    dp.mark(0);
    if (ibBytes) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ibBytes, ib, GL_STREAM_DRAW);
    }
    dp.mark(1);
    return true;
}

// --- the three pipelines, addressed by byte offset into the resident stream buffers ------
// vbOfs/ibOfs are byte offsets into the CURRENT contents of vbo/ibo. Indices stay draw-local
// (0..vertexCount-1) because the attribute pointers are re-based to vbOfs -- so no baseVertex
// arithmetic, and therefore no chance of getting it wrong, is involved.
void GlesDrawPCAt(uint32_t vbOfs, int vertexCount) {
    Device::Impl* p = (Device::Impl*)g_implV;
    if (!p || vertexCount <= 0) return;
    DiagPhase dp;
    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    dp.mark(0); dp.mark(1);
    p->ApplyWVP(p->color);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexPC), (void*)(uintptr_t)(vbOfs + 0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VertexPC), (void*)(uintptr_t)(vbOfs + 12));
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    dp.mark(2);
    if (!(g_diag & 1u)) glDrawArrays(GL_TRIANGLES, 0, vertexCount);
    dp.mark(3);
    ++g_diagDraws;
}

void GlesDrawPTCAt(uint32_t vbOfs, int vertexCount, uint32_t ibOfs, int indexCount) {
    Device::Impl* p = (Device::Impl*)g_implV;
    if (!p || vertexCount <= 0 || indexCount <= 0) return;
    DiagPhase dp;
    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p->ibo);
    dp.mark(0); dp.mark(1);
    p->ApplyWVP(p->tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->TexId(p->curTex));
    glBindSampler(0, p->curSamp);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexPTC), (void*)(uintptr_t)(vbOfs + 0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VertexPTC), (void*)(uintptr_t)(vbOfs + 12));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VertexPTC), (void*)(uintptr_t)(vbOfs + 20));
    glDisableVertexAttribArray(3);
    dp.mark(2);
    if (!(g_diag & 1u)) glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, (void*)(uintptr_t)ibOfs);
    dp.mark(3);
    ++g_diagDraws;
}

void GlesDrawShinyAt(uint32_t vbOfs, int vertexCount, uint32_t ibOfs, int indexCount,
                     TextureHandle t0, TextureHandle t1, TextureHandle t2, TextureHandle t3) {
    Device::Impl* p = (Device::Impl*)g_implV;
    if (!p || vertexCount <= 0 || indexCount <= 0) return;
    DiagPhase dp;
    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p->ibo);
    dp.mark(0); dp.mark(1);
    p->ApplyWVP(p->shiny);
    TextureHandle ts[4] = { t0, t1, t2, t3 };
    for (int i = 0; i < 4; ++i) { glActiveTexture(GL_TEXTURE0 + i);
                                  glBindTexture(GL_TEXTURE_2D, p->TexId(ts[i]));
                                  glBindSampler(i, p->curSamp); }
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexPT2C), (void*)(uintptr_t)(vbOfs + 0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VertexPT2C), (void*)(uintptr_t)(vbOfs + 12));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(VertexPT2C), (void*)(uintptr_t)(vbOfs + 20));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VertexPT2C), (void*)(uintptr_t)(vbOfs + 28));
    dp.mark(2);
    if (!(g_diag & 1u)) glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, (void*)(uintptr_t)ibOfs);
    dp.mark(3);
    ++g_diagDraws;
    glActiveTexture(GL_TEXTURE0);   // leave unit 0 active for the single-texture paths
}

// --- touch-control overlay ----------------------------------------------------------------
// Drawn by the compositor between the frame's replay and the swap, directly on the default
// framebuffer in FULL SURFACE pixel coordinates (origin top-left) — the touch zones cover
// the whole window including the pillarbox bars, so the game's 16:9 present rect is the
// wrong space for it. The ring stream only carries state CHANGES, so whatever this touches
// (viewport, blend, depth, the cached WVP) must be put back exactly, or the NEXT frame's
// first draws inherit overlay state the game never asked for.
void GlesDrawOverlay(const VertexPC* verts, int count) {
    Device::Impl* p = (Device::Impl*)g_implV;
    if (!p || !verts || count <= 0) return;
    // A frame always ends on the backbuffer; if it somehow did not, skip the overlay for a
    // frame rather than re-binding FBO 0 behind the device's render-target bookkeeping.
    if (p->activeRT >= 0) return;
    GLboolean blendOn = glIsEnabled(GL_BLEND);
    GLboolean depthOn = glIsEnabled(GL_DEPTH_TEST);
    GLboolean depthMask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    GLint bsr = GL_ONE, bdr = GL_ZERO, bsa = GL_ONE, bda = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsr);   glGetIntegerv(GL_BLEND_DST_RGB, &bdr);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsa); glGetIntegerv(GL_BLEND_DST_ALPHA, &bda);
    float saveWvp[16]; memcpy(saveWvp, p->wvp, sizeof saveWvp);
    int saveFlip = p->flip;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);        // always the window, never a game RT
    glViewport(0, 0, p->w, p->h);
    // Pixel-space ortho (row-major, shader does v*M): x -> [-1,1], y top-down -> [1,-1].
    memset(p->wvp, 0, sizeof p->wvp);
    p->wvp[0] = 2.0f / (float)p->w;  p->wvp[12] = -1.0f;
    p->wvp[5] = -2.0f / (float)p->h; p->wvp[13] = 1.0f;
    p->wvp[10] = 1.0f;               p->wvp[15] = 1.0f;
    p->flip = 0;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    // Orphaning re-specification, same as every other upload here. The frame's own draws are
    // already issued (the overlay runs after the OP_PRESENT that ends the frame), and the
    // batched path re-uploads the whole stream next frame, so clobbering the buffer is safe.
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(VertexPC) * count, verts, GL_STREAM_DRAW);
    p->ApplyWVP(p->color);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexPC), (void*)(uintptr_t)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VertexPC), (void*)(uintptr_t)12);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDrawArrays(GL_TRIANGLES, 0, count);

    memcpy(p->wvp, saveWvp, sizeof saveWvp);
    p->flip = saveFlip;
    if (blendOn) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFuncSeparate((GLenum)bsr, (GLenum)bdr, (GLenum)bsa, (GLenum)bda);
    if (depthOn) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(depthMask);
    p->BackbufferViewport();
}

// --- the d3d8.h entry points: upload this draw, then run the same body -------------------
void Device::DrawTriangleList(const VertexPC* verts, int vertexCount) {
    if (!p_ || vertexCount <= 0) return;
    DiagPhase dp;
    glBindVertexArray(p_->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p_->vbo);
    if (g_diag & 2u) DiagEnsureResidentBuffers(p_->vbo, p_->ibo);
    else glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(VertexPC) * vertexCount, verts, GL_STREAM_DRAW);
    dp.mark(0);
    dp.mark(1);                                  // no index buffer on this path
    GlesDrawPCAt(0, vertexCount);
}

void Device::DrawIndexed(const VertexPTC* verts, int vertexCount,
                         const uint16_t* indices, int indexCount) {
    if (!p_ || vertexCount <= 0 || indexCount <= 0) return;
    DiagPhase dp;
    glBindVertexArray(p_->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p_->vbo);
    if (g_diag & 2u) DiagEnsureResidentBuffers(p_->vbo, p_->ibo);
    else glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(VertexPTC) * vertexCount, verts, GL_STREAM_DRAW);
    dp.mark(0);
    if (!(g_diag & 2u)) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p_->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(uint16_t) * indexCount, indices, GL_STREAM_DRAW);
    }
    dp.mark(1);
    GlesDrawPTCAt(0, vertexCount, 0, indexCount);
}

void Device::DrawShinyIndexed(const VertexPT2C* verts, int vertexCount,
                              const uint16_t* indices, int indexCount,
                              TextureHandle t0, TextureHandle t1,
                              TextureHandle t2, TextureHandle t3) {
    if (!p_ || vertexCount <= 0 || indexCount <= 0) return;
    DiagPhase dp;
    glBindVertexArray(p_->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p_->vbo);
    if (g_diag & 2u) DiagEnsureResidentBuffers(p_->vbo, p_->ibo);
    else glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(VertexPT2C) * vertexCount, verts, GL_STREAM_DRAW);
    dp.mark(0);
    if (!(g_diag & 2u)) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p_->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(uint16_t) * indexCount, indices, GL_STREAM_DRAW);
    }
    dp.mark(1);
    GlesDrawShinyAt(0, vertexCount, 0, indexCount, t0, t1, t2, t3);
}

// --- Textures ---------------------------------------------------------------------------
TextureHandle Device::CreateTexture(const uint32_t* rgba, int width, int height) {
    if (!p_ || width <= 0 || height <= 0) return kNoTexture;
    // NO row flip (false), proven by the on-device title screen (session 28): texel v=0 =
    // data row 0 in BOTH D3D and GL — bottom-up is a window/readback convention, not a
    // sampler one. The first pass flipped here and every texture rendered mirrored in place.
    GLuint id = UploadAssetTexture(rgba, width, height, false);
    if (!id) return kNoTexture;
    ++p_->liveTextures;
    return p_->AllocSlot(id, width, height, false);
}

bool Device::UpdateTexture(TextureHandle t, const uint32_t* rgba, int width, int height, bool genMips) {
    if (!p_ || t < 0 || t >= (int)p_->texs.size() || !p_->texs[t].id) return false;
    if (p_->texs[t].w != width || p_->texs[t].h != height) return false;
    ReuploadAssetTexture(p_->texs[t].id, rgba, width, height, false, genMips);
    return true;
}

void Device::DestroyTexture(TextureHandle t) {
    if (!p_ || t < 0 || t >= (int)p_->texs.size() || !p_->texs[t].id) return;
    if (t == p_->activeRT) return;
    if (t == p_->curTex) p_->curTex = kNoTexture;
    Impl::Tex& s = p_->texs[t];
    glDeleteTextures(1, &s.id);
    if (s.fbo) glDeleteFramebuffers(1, &s.fbo);
    if (s.depthRb) glDeleteRenderbuffers(1, &s.depthRb);
    s = Impl::Tex{};
    p_->texFree.push_back(t);
    --p_->liveTextures;
}

int Device::TextureCount() const { return p_ ? p_->liveTextures : 0; }
int Device::TexturePooled() const { return p_ ? p_->pooledTextures : 0; }

TextureHandle Device::AcquireTexture(const uint32_t* rgba, int width, int height) {
    if (!p_ || width <= 0 || height <= 0) return kNoTexture;
    uint64_t key = ((uint64_t)(uint32_t)width << 32) | (uint32_t)height;
    auto it = p_->texPool.find(key);
    while (it != p_->texPool.end() && !it->second.empty()) {
        TextureHandle h = it->second.back(); it->second.pop_back();
        --p_->pooledTextures;
        if (h < 0 || h >= (int)p_->texs.size() || !p_->texs[h].id) continue;
        ReuploadAssetTexture(p_->texs[h].id, rgba, width, height, false, true);
        ++p_->liveTextures;
        return h;
    }
    return CreateTexture(rgba, width, height);
}

void Device::ReleaseTexture(TextureHandle t) {
    if (!p_ || t < 0 || t >= (int)p_->texs.size() || !p_->texs[t].id) return;
    if (t == p_->activeRT || p_->texs[t].isRT) { DestroyTexture(t); return; }
    if (t == p_->curTex) p_->curTex = kNoTexture;
    uint64_t key = ((uint64_t)(uint32_t)p_->texs[t].w << 32) | (uint32_t)p_->texs[t].h;
    auto& bucket = p_->texPool[key];
    if ((int)bucket.size() >= 32) { DestroyTexture(t); return; }
    bucket.push_back(t);
    --p_->liveTextures; ++p_->pooledTextures;
}

// --- Render-to-texture (FBO) ------------------------------------------------------------
TextureHandle Device::CreateRenderTexture(int width, int height) {
    if (!p_ || width <= 0 || height <= 0) return kNoTexture;
    GLuint id = 0; glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);      // establish the mip chain for later regen
    GLuint fbo = 0; glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, id, 0);
    GLuint drb = 0; glGenRenderbuffers(1, &drb);
    glBindRenderbuffer(GL_RENDERBUFFER, drb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, drb);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ++p_->liveTextures;
    TextureHandle h = p_->AllocSlot(id, width, height, true);
    p_->texs[h].fbo = fbo; p_->texs[h].depthRb = drb;
    return h;
}

void Device::SetRenderTexture(TextureHandle t) {
    if (!p_ || t < 0 || t >= (int)p_->texs.size() || !p_->texs[t].fbo) return;
    glBindFramebuffer(GL_FRAMEBUFFER, p_->texs[t].fbo);
    glViewport(0, 0, p_->texs[t].w, p_->texs[t].h);
    p_->activeRT = t;
    // GL FBOs are bottom-up but the game samples RTs with D3D top-origin UVs: negate
    // clip-space Y while rendering in (gFlip; cull is off, so winding is safe) so texel
    // row 0 holds the image TOP, exactly like a D3D render target.
    p_->flip = 1;
}

void Device::SetRenderTargetBackbuffer() {
    if (!p_) return;
    int prev = p_->activeRT;
    p_->activeRT = -1;
    p_->flip = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_->BackbufferViewport();
    if (prev >= 0 && prev < (int)p_->texs.size() && p_->texs[prev].id) {
        glBindTexture(GL_TEXTURE_2D, p_->texs[prev].id);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
}

TextureHandle Device::CreateCaptureTexture() {
    if (!p_) return kNoTexture;
    // Captures are GAME-sized (the present rect), not surface-sized: the game side sizes
    // its capture bookkeeping to the render resolution, and the pillars are not content.
    int cw = p_->prW > 0 ? p_->prW : p_->w;
    int ch = p_->prW > 0 ? p_->prH : p_->h;
    GLuint id = 0; glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cw, ch, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // NO MIP CHAIN. A capture is only ever drawn back as a full-screen background at 1:1,
    // so no sampler reads a level below 0 -- but the chain was REGENERATED on every
    // presented frame in CopyBackbufferTo: at 2240x1260 that is ~2.8 Mpx of blit plus an
    // 11-level filtered chain, ~30 MB of GPU traffic and ~12 render-pass setups PER FRAME,
    // on levels nothing samples. MAX_LEVEL=0 makes the texture complete with level 0 alone.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    // A color-only FBO wrapping the texture, so CopyBackbufferTo can blit Y-inverted
    // (D3D top-origin content) instead of glCopyTexSubImage2D's bottom-up copy.
    GLuint fbo = 0; glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, id, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, p_->activeRT >= 0 ? p_->texs[p_->activeRT].fbo : 0);
    ++p_->liveTextures;
    TextureHandle h = p_->AllocSlot(id, cw, ch, true);  // isRT: freed (not pooled) on release
    p_->texs[h].fbo = fbo;                              // no depth: capture-only FBO
    return h;
}

void Device::CopyBackbufferTo(TextureHandle t) {
    if (!p_ || t < 0 || t >= (int)p_->texs.size() || !p_->texs[t].id) return;
    Impl::Tex& tex = p_->texs[t];
    int sx = p_->prW > 0 ? p_->prX : 0, sy = p_->prW > 0 ? p_->prY : 0;
    int sw = p_->prW > 0 ? p_->prW : p_->w, sh = p_->prW > 0 ? p_->prH : p_->h;
    if (tex.fbo) {
        // Y-inverting blit: default-FB rows are bottom-up, the game samples captures with
        // D3D top-origin UVs — swap the source Y range so texel row 0 = screen top.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tex.fbo);
        glBlitFramebuffer(sx, sy + sh, sx + sw, sy,           // src top-down
                          0, 0, tex.w, tex.h,                 // dst
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, p_->activeRT >= 0 ? p_->texs[p_->activeRT].fbo : 0);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, tex.id);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sx, sy,
                            sw < tex.w ? sw : tex.w, sh < tex.h ? sh : tex.h);
    }
    // (No glGenerateMipmap here: see CreateCaptureTexture -- level 0 is all anything reads.)
}

bool Device::ResizeBackbuffer(int width, int height) {
    if (!p_ || width < 1 || height < 1) return false;
    p_->w = width; p_->h = height;   // the EGL window surface tracks the ANativeWindow size
    if (p_->activeRT < 0) glViewport(0, 0, width, height);
    return true;
}
bool Device::SetFullscreenExclusive(bool, int, int) { return false; }   // always fullscreen on Android

void Device::SetTexture(TextureHandle t) { if (p_) p_->curTex = t; }
void Device::SetUvClamp(bool clampU, bool clampV) {
    if (p_) p_->curSamp = p_->samp[clampU ? 1 : 0][clampV ? 1 : 0];
}

void Device::SetDepthTest(bool enable) {
    if (!p_) return;
    if (enable) { glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDepthFunc(GL_LEQUAL); }
    else        { glDisable(GL_DEPTH_TEST); }
}

void Device::SetAlphaBlend(bool enable) {
    if (!p_) return;
    if (enable) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);      // test on, write off (alpha pass)
    } else {
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
    }
}

void Device::SetBlendMode(BlendMode mode, bool depthWrite) {
    if (!p_) return;
    if (mode == BLEND_OPAQUE) glDisable(GL_BLEND);
    else {
        glEnable(GL_BLEND);
        if (mode == BLEND_ADD) glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
        else                   glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    glEnable(GL_DEPTH_TEST); glDepthMask(depthWrite ? GL_TRUE : GL_FALSE); glDepthFunc(GL_LEQUAL);
}

// Diagnostic BMP dump of the current backbuffer (mirrors d3d8.cpp SaveBackbuffer).
bool Device::SaveBackbuffer(const char* path) {
    if (!p_) return false;
    int W = p_->w, H = p_->h;
    std::vector<uint8_t> px((size_t)W * H * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());   // rows bottom-up (GL)
    int rowsz = W * 3, pad = (4 - (rowsz & 3)) & 3, stride = rowsz + pad, imgsz = stride * H;
    unsigned char hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M'; *(int*)(hdr+2)=54+imgsz; *(int*)(hdr+10)=54;
    *(int*)(hdr+14)=40; *(int*)(hdr+18)=W; *(int*)(hdr+22)=H; hdr[26]=1; hdr[28]=24; *(int*)(hdr+34)=imgsz;
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> row(stride, 0);
    for (int y = 0; y < H; ++y) {                    // BMP is bottom-up; GL data already is
        const uint8_t* src = &px[(size_t)y * W * 4];
        memset(row.data(), 0, stride);
        for (int x = 0; x < W; ++x) { row[x*3+0]=src[x*4+2]; row[x*3+1]=src[x*4+1]; row[x*3+2]=src[x*4+0]; }
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    return true;
}

// ---- App-lifecycle surface handoff (Android compositor use only) -------------------------
// The ANativeWindow dies on APP_CMD_TERM_WINDOW and a NEW one arrives on the next
// INIT_WINDOW; the EGL context (all textures/programs) must survive the gap. These free
// functions operate through g_appEgl (captured at Create) so the shared d3d8.h interface —
// which the D3D11 and null backends also implement — stays untouched.
bool GlesReleaseWindowSurface() {
    if (g_appEgl.dpy == EGL_NO_DISPLAY || !g_appEgl.surf) return false;
    // Keep the context current surfaceless (EGL_KHR_surfaceless_context is universal on
    // GLES3 hardware); if the driver refuses, unbind entirely — Attach re-binds.
    if (!eglMakeCurrent(g_appEgl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_appEgl.ctx))
        eglMakeCurrent(g_appEgl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (*g_appEgl.surf != EGL_NO_SURFACE) {
        eglDestroySurface(g_appEgl.dpy, *g_appEgl.surf);
        *g_appEgl.surf = EGL_NO_SURFACE;
    }
    printf("[gles] window surface released (context kept)\n");
    return true;
}

bool GlesAttachWindowSurface(void* nativeWindow) {
    if (g_appEgl.dpy == EGL_NO_DISPLAY || !g_appEgl.surf || !nativeWindow) return false;
    if (*g_appEgl.surf != EGL_NO_SURFACE) {
        eglDestroySurface(g_appEgl.dpy, *g_appEgl.surf);
        *g_appEgl.surf = EGL_NO_SURFACE;
    }
    ANativeWindow* win = (ANativeWindow*)nativeWindow;
    EGLint fmt = 0; eglGetConfigAttrib(g_appEgl.dpy, g_appEgl.cfg, EGL_NATIVE_VISUAL_ID, &fmt);
    ANativeWindow_setBuffersGeometry(win, 0, 0, fmt);
    EGLSurface s = eglCreateWindowSurface(g_appEgl.dpy, g_appEgl.cfg, win, nullptr);
    if (s == EGL_NO_SURFACE) { printf("[gles] attach: eglCreateWindowSurface failed\n"); return false; }
    if (!eglMakeCurrent(g_appEgl.dpy, s, s, g_appEgl.ctx)) {
        printf("[gles] attach: eglMakeCurrent failed\n");
        eglDestroySurface(g_appEgl.dpy, s);
        return false;
    }
    *g_appEgl.surf = s;
    eglSwapInterval(g_appEgl.dpy, g_appEgl.vsync ? 1 : 0);
    EGLint sw = 0, sh = 0;
    eglQuerySurface(g_appEgl.dpy, s, EGL_WIDTH, &sw);
    eglQuerySurface(g_appEgl.dpy, s, EGL_HEIGHT, &sh);
    if (sw > 0 && sh > 0) { *g_appEgl.w = sw; *g_appEgl.h = sh; }
    // Recompute the centered present rect for the (possibly resized) surface, then aim the
    // viewport at it — the game keeps rendering its fixed 16:9 size regardless.
    if (g_appEgl.prW && *g_appEgl.prW > 0) {
        *g_appEgl.prX = (*g_appEgl.w - *g_appEgl.prW) / 2;
        *g_appEgl.prY = (*g_appEgl.h - *g_appEgl.prH) / 2;
        if (*g_appEgl.prX < 0) *g_appEgl.prX = 0;
        if (*g_appEgl.prY < 0) *g_appEgl.prY = 0;
        glViewport(*g_appEgl.prX, *g_appEgl.prY, *g_appEgl.prW, *g_appEgl.prH);
    } else {
        glViewport(0, 0, *g_appEgl.w, *g_appEgl.h);
    }
    printf("[gles] window surface attached %dx%d\n", *g_appEgl.w, *g_appEgl.h);
    return true;
}

} // namespace tj::gfx

extern "C" uint32_t Device_NullSrvBinds() { return 0; }
