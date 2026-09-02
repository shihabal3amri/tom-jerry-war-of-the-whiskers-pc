// Xbox Direct3D 8 -> Direct3D 11 translation (first slice).
//
// The Xbox exposes an inline D3D8 API (D3DDevice_Clear, D3DDevice_SetRenderState_*,
// D3DDevice_DrawVerticesUP, ...). This layer implements the operations the game's
// renderer actually uses, backed by a modern Direct3D 11 device — the foundation that
// lets the ported game draw natively. This first slice covers device bring-up, the
// render-state cache, clears, scene begin/end/present, and immediate-mode triangle
// drawing (XYZ + DIFFUSE), enough to render real geometry.
#pragma once
#include <cstdint>

struct HWND__; using HWND = HWND__*;
struct ID3D11Device;

namespace tj::gfx {

struct PresentParams {
    int  backWidth   = 1280;
    int  backHeight  = 720;
    bool vsync       = true;
    int  sampleCount = 1;     // MSAA (1/2/4/8); the engine picks this from its quality setting
};

struct DisplayMode { int width, height, refreshHz; };

// Enumerate the primary output's supported modes (deduplicated, ascending). Static: this
// is adapter info, available before a device exists (as the engine's mode scan needs).
int  EnumDisplayModes(DisplayMode* out, int maxOut);
DisplayMode DesktopMode();
int  MaxMsaaSampleCount();   // highest MSAA the adapter supports for the back-buffer format

// D3DFVF-style vertices the game's paths use.
struct VertexPC  { float x, y, z; uint32_t color; };                 // XYZ | DIFFUSE
struct VertexPTC { float x, y, z; float u, v; uint32_t color; };     // XYZ | TEX1 | DIFFUSE
// Shiny-floor vertex: base UV + CPU-computed environment (reflection) UV.
struct VertexPT2C { float x, y, z; float u0, v0; float u1, v1; uint32_t color; };

// Opaque handle to a runtime texture (an index into the device's texture table).
using TextureHandle = int;
constexpr TextureHandle kNoTexture = -1;

// Xbox D3DCLEAR flags (subset).
enum : uint32_t { CLEAR_TARGET = 0x1, CLEAR_ZBUFFER = 0x2, CLEAR_STENCIL = 0x4 };

class Device {
public:
    bool Create(HWND hwnd, const PresentParams& pp);
    void Shutdown();

    void Clear(uint32_t flags, uint32_t argb, float z, uint8_t stencil);
    void BeginScene();
    void EndScene();
    void Present();

    // Row-major 4x4 world-view-projection applied to draws.
    void SetTransform(const float m[16]);
    void DrawTriangleList(const VertexPC* verts, int vertexCount);

    // --- Textured / indexed geometry (real game meshes) ---
    // Create a texture from 32-bit RGBA pixels (row-major, w*h). Returns a handle.
    // Destroyed slots are reused, so handles stay small; callers that cache handles must
    // treat DestroyTexture as invalidating them.
    TextureHandle CreateTexture(const uint32_t* rgbaPixels, int width, int height);
    // Re-upload pixels into an EXISTING texture (animated textures: water, flipbooks).
    // Reuses the GPU allocation instead of creating a new texture every frame -- the
    // per-frame create/destroy churn was the "ship map slow motion". Returns false if
    // the handle is invalid or the dimensions changed (caller should recreate).
    bool UpdateTexture(TextureHandle tex, const uint32_t* rgbaPixels, int width, int height,
                       bool genMips = true);
    void DestroyTexture(TextureHandle tex);
    // Recycling pool: Acquire reuses a released same-dimension texture (via in-place
    // upload) instead of allocating a new one; Release returns a texture to the pool
    // instead of freeing it. This eliminates the per-frame create/destroy churn of
    // animated world textures (sea water etc.) -- the churn caused slow motion, stale-
    // handle black rendering, and GPU-memory exhaustion. Handles from Acquire are valid
    // until Released; a Released handle keeps its GPU allocation alive (never dangles).
    TextureHandle AcquireTexture(const uint32_t* rgbaPixels, int width, int height);
    void ReleaseTexture(TextureHandle tex);
    int  TextureCount() const;               // in-use textures (leak diagnostics)
    int  TexturePooled() const;              // pooled (reusable) textures
    // --- Render-to-texture (the game's per-pass RT redirect: motion-blur smears,
    // level-select previews). A render texture is a normal texture handle that can also
    // be bound as the draw target; it gets its own size-matched depth buffer.
    TextureHandle CreateRenderTexture(int width, int height);
    void SetRenderTexture(TextureHandle tex);   // redirect subsequent draws + clears
    void SetRenderTargetBackbuffer();           // restore; regenerates the RT's mips
    // Backbuffer snapshot (the game's screen-transition capture): create a texture the
    // size/format of the backbuffer and copy the current backbuffer contents into it.
    TextureHandle CreateCaptureTexture();
    void CopyBackbufferTo(TextureHandle tex);
    // Live resolution change (in-game resolution option): resize the swapchain buffers
    // and rebuild the render/depth targets. Capture textures must be recreated by the
    // caller (their size is fixed at creation).
    bool ResizeBackbuffer(int width, int height);
    // Exclusive (DXGI) fullscreen. Entering also retargets the output mode to w x h and
    // resizes the buffers; leaving returns to windowed (caller restyles/sizes the window).
    bool SetFullscreenExclusive(bool on, int width, int height);
    void SetTexture(TextureHandle tex);               // kNoTexture -> untextured (white)
    void SetUvClamp(bool clampU, bool clampV);        // per-draw texture address mode
    void SetDepthTest(bool enable);
    // Alpha blending (SrcAlpha/InvSrcAlpha). While enabled, depth is tested but
    // not written — the standard transparent-pass setup.
    void SetAlphaBlend(bool enable);
    // Full blend/depth-write control for the bridged game render states.
    enum BlendMode { BLEND_OPAQUE = 0, BLEND_ALPHA = 1, BLEND_ADD = 2 };
    void SetBlendMode(BlendMode mode, bool depthWrite);
    void DrawIndexed(const VertexPTC* verts, int vertexCount,
                     const uint16_t* indices, int indexCount);
    // The Shiny 4-stage combiner (Kitchen/Lab/Market floor sheen), exact Xbox formula:
    //   rgb = t0(uv0)*vcol + t2(uv1)*t1(uv0) + t3(uv1)*t1(uv0)*0.5,  a = t0.a*vcol.a
    void DrawShinyIndexed(const VertexPT2C* verts, int vertexCount,
                          const uint16_t* indices, int indexCount,
                          TextureHandle t0, TextureHandle t1,
                          TextureHandle t2, TextureHandle t3);

    ID3D11Device* D3D11() const;
    bool Valid() const;
    bool SaveBackbuffer(const char* path);   // dump back buffer to a BMP (diagnostic)

private:
    struct Impl;
    Impl* p_ = nullptr;

    // ANDROID/GLES ONLY. The compositor uploads a whole frame of geometry in one
    // glBufferData (the per-draw upload was measured at 78% of in-match draw cost) and then
    // issues draws that name their slice by byte offset. Those entry points live in
    // gles_gfx.cpp as free functions rather than as methods, so the D3D11 backend is not
    // obliged to implement a path it does not need — which is why they are friends here.
    // Declarations only: no members, no virtuals, nothing the Windows build codegens.
    friend bool GlesStreamUpload(const void*, unsigned long, const void*, unsigned long);
    friend void GlesDrawPCAt(uint32_t, int);
    friend void GlesDrawPTCAt(uint32_t, int, uint32_t, int);
    friend void GlesDrawShinyAt(uint32_t, int, uint32_t, int,
                                TextureHandle, TextureHandle, TextureHandle, TextureHandle);
    // Touch-control overlay: verts in SURFACE pixel coords (full window, pillars included —
    // the touch zones live in surface space, not the game's 16:9 rect). Saves and restores
    // every piece of GL state it touches, because the ring only sends state CHANGES.
    friend void GlesDrawOverlay(const VertexPC*, int);
};

} // namespace tj::gfx
