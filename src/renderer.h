#pragma once

// renderer.h — Direct3D 11 renderer. The PUBLIC API is fixed by the architect; DO NOT CHANGE IT.
// The "private:" section and renderer.cpp belong to the "renderer" unit.
//
// SCENE / CAMERA CONTRACT
// -------------------------
// * Scene space is defined in model.h: mm, right-handed, Z UP, build plate plane z = 0, model centred in XY.
// * The camera is orthographic and uses RIGHT-HANDED matrices (XMMatrixLookAtRH + XMMatrixOrthographicOffCenterRH),
//   so the image is not mirrored (front view: +X is right on screen, +Z is up).
// * Orbit centre = centre of the model bounding box (0, 0, sz/2).
// * azimuthDeg / elevationDeg are defined EXACTLY by ViewDirection() / ViewUp() (below).
// * Fit is computed from the MODEL; grid lines do not affect it. Exceptions:
//   with showBBox on, the 8 bounding box corners (so the box and X/Y/Z labels stay inside the window); with
//   showPlate on, the 4 corners of the K1 Max plate also take part (at zoom 1 the whole plate is visible).
//   Both are off for thumbnails: the fit is based on the model only.

#include <d3d11.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

#include <wrl/client.h>

#include "model.h"

// ---------------------------------------------------------------------------------------------
// Process-wide shared Direct3D 11 device
// ---------------------------------------------------------------------------------------------
// Preview windows / thumbnails do not create their own device (per-device event handles and driver
// memory): one device is shared with a reference count. Each window only has its swap chain, Renderer
// resources and D2D device context.
// The immediate context is NOT thread-safe: every call that uses it (Render, Blit,
// ReadPixels, Overlay::Draw, ResizeBuffers, Present, ClearState) runs under SharedGpuMutex().
std::recursive_mutex& SharedGpuMutex();
// Returns the shared device and increments the reference count. If there is no device or it was lost (TDR, driver
// update) a new one is created (holders of the old device keep working with their own references and ask again
// when they see the error). Returns false on failure (count unchanged).
bool AcquireSharedGpuDevice(Microsoft::WRL::ComPtr<ID3D11Device>& device);
// Counterpart of AcquireSharedGpuDevice. The device stays cached even when the count drops to zero (re-creating
// it leaks handles); resources holding GPU memory belong to the users and are released with them.
void ReleaseSharedGpuDevice();
// Test/measurement: number of shared device references currently alive.
long SharedGpuDeviceUsers();

// ---------------------------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------------------------

inline constexpr float kDefaultAzimuthDeg = 38.0f;    // isometric: camera front-right-above (+X, -Y, +Z)
inline constexpr float kDefaultElevationDeg = 24.0f;
inline constexpr float kMinZoom = 0.2f;
inline constexpr float kMaxZoom = 40.0f;

struct CameraState {
    // Camera angle around the Z axis. 0 = front view (camera on the -Y side, looking toward +Y,
    // +X right on screen). 90 = from the right (camera on the +X side). 180 = from the back. -90 = from the left.
    float azimuthDeg = kDefaultAzimuthDeg;
    // Elevation angle from the plate plane, [-90, +90]. +90 = from above, -90 = from below.
    float elevationDeg = kDefaultElevationDeg;
    // 1 = model fits the window. Larger values zoom in. Clamped to [kMinZoom, kMaxZoom].
    float zoom = 1.0f;
    // Pan in MILLIMETRES on the view plane: content moves right (+panX) / up (+panY) on screen.
    // (Use Renderer::LastFrame().pixelsPerMm to convert a mouse delta to mm.)
    float panX = 0.0f;
    float panY = 0.0f;
    // Background colour (0..1). Light/dark theme is derived from it: IsDarkBackground().
    DirectX::XMFLOAT3 background{0.96f, 0.965f, 0.975f};
};

enum class ViewPreset {
    Isometric,  // key 0 : default view (az 38, el 24)
    Front,      // key 1 : front  (az 0,   el 0)
    Back,       // key 2 : back   (az 180, el 0)
    Left,       // key 3 : left   (az -90, el 0)   camera on the -X side
    Right,      // key 4 : right  (az 90,  el 0)   camera on the +X side
    Top,        // key 5 : top    (az 0,   el 90)  +X right, +Y up on screen
    Bottom,     // key 6 : bottom (az 0,   el -90) +X right, +Y down on screen
};

// Sets the angles to the preset; sets zoom = 1, pan = 0. Leaves background untouched.
inline void ApplyViewPreset(CameraState& camera, ViewPreset preset) {
    float azimuth = kDefaultAzimuthDeg;
    float elevation = kDefaultElevationDeg;
    switch (preset) {
        case ViewPreset::Isometric: break;
        case ViewPreset::Front:  azimuth = 0.0f;   elevation = 0.0f;   break;
        case ViewPreset::Back:   azimuth = 180.0f; elevation = 0.0f;   break;
        case ViewPreset::Left:   azimuth = -90.0f; elevation = 0.0f;   break;
        case ViewPreset::Right:  azimuth = 90.0f;  elevation = 0.0f;   break;
        case ViewPreset::Top:    azimuth = 0.0f;   elevation = 90.0f;  break;
        case ViewPreset::Bottom: azimuth = 0.0f;   elevation = -90.0f; break;
    }
    camera.azimuthDeg = azimuth;
    camera.elevationDeg = elevation;
    camera.zoom = 1.0f;
    camera.panX = 0.0f;
    camera.panY = 0.0f;
}

// Unit vector from the orbit centre TOWARD THE CAMERA (scene space).
//   d = ( sin(az)·cos(el), -cos(az)·cos(el), sin(el) )
inline DirectX::XMFLOAT3 ViewDirection(float azimuthDeg, float elevationDeg) {
    const float az = DirectX::XMConvertToRadians(azimuthDeg);
    const float el = DirectX::XMConvertToRadians(std::clamp(elevationDeg, -90.0f, 90.0f));
    return {std::sin(az) * std::cos(el), -std::cos(az) * std::cos(el), std::sin(el)};
}

// Camera "up" vector (derivative of d with respect to el). Also defined for el = ±90,
// so it is ALWAYS passed to LookAt instead of a fixed (0,0,1).
//   u = ( -sin(az)·sin(el), cos(az)·sin(el), cos(el) )
inline DirectX::XMFLOAT3 ViewUp(float azimuthDeg, float elevationDeg) {
    const float az = DirectX::XMConvertToRadians(azimuthDeg);
    const float el = DirectX::XMConvertToRadians(std::clamp(elevationDeg, -90.0f, 90.0f));
    return {-std::sin(az) * std::sin(el), std::cos(az) * std::sin(el), std::cos(el)};
}

inline bool IsDarkBackground(const DirectX::XMFLOAT3& background) {
    return (0.2126f * background.x + 0.7152f * background.y + 0.0722f * background.z) < 0.5f;
}

// ---------------------------------------------------------------------------------------------
// Measurement grid layout — pure function; the renderer and the overlay use the same result
// ---------------------------------------------------------------------------------------------

inline constexpr float kPlateSizeMm = 300.0f;   // Creality K1 Max plate 300 × 300 mm

struct GridLayout {
    // Major line spacing (mm). Normally 10; 100, 1000... for very large models.
    float stepMm = 10.0f;
    // Every strongEvery-th line is drawn strong (50 mm with a 10 mm step).
    int strongEvery = 5;
    // Fine line spacing = stepMm / 10 (normally 1 mm). Drawn only when zoomed in far enough.
    float fineStepMm = 1.0f;
    // ZERO of the ruler: the model's (minX, minY) corner in scene space. Lines are counted from here;
    // the model starts at the zero line and its size can be read by counting squares.
    float originX = 0.0f;
    float originY = 0.0f;
    // Line index ranges:
    //   constant-x lines: x = originX + k·stepMm, k ∈ [-lowX, highX]
    //   constant-y lines: y = originY + k·stepMm, k ∈ [-lowY, highY]
    // k % strongEvery == 0 -> strong line; k == 0 -> zero (origin) line.
    int lowX = 1, highX = 5;
    int lowY = 1, highY = 5;
    // Extent of the grid in scene space (derived from the above).
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
};

// bounds: Mesh::bounds (scene space). Extent: the footprint is rounded up to a multiple of the step, at least
// one step of margin on each side, at least 5 steps per edge (50 × 50 mm with a 10 mm step).
inline GridLayout ComputeGridLayout(const float bounds[6]) {
    GridLayout g;
    const float sizeX = std::max(bounds[3] - bounds[0], 0.0f);
    const float sizeY = std::max(bounds[4] - bounds[1], 0.0f);
    const float largest = std::max(sizeX, sizeY);
    g.stepMm = 10.0f;
    while (largest / g.stepMm > 200.0f && g.stepMm < 1.0e6f) g.stepMm *= 10.0f;
    g.fineStepMm = g.stepMm / 10.0f;
    g.strongEvery = 5;
    g.originX = bounds[0];
    g.originY = bounds[1];
    const float step = g.stepMm;
    auto span = [step](float size, int& low, int& high) {
        const int cells = std::max(1, static_cast<int>(std::ceil(size / step - 1.0e-4f)));
        const int total = std::max(cells + 2, 5);
        low = std::max(1, (total - cells) / 2);
        high = total - low;
    };
    span(sizeX, g.lowX, g.highX);
    span(sizeY, g.lowY, g.highY);
    g.minX = g.originX - static_cast<float>(g.lowX) * g.stepMm;
    g.maxX = g.originX + static_cast<float>(g.highX) * g.stepMm;
    g.minY = g.originY - static_cast<float>(g.lowY) * g.stepMm;
    g.maxY = g.originY + static_cast<float>(g.highY) * g.stepMm;
    return g;
}

// Draw fine (1 mm) lines? pixelsPerMm: on-screen scale; lineScale: RenderOptions::lineScale.
// Rule: fine step >= 6·lineScale pixels on screen AND fine lines per direction <= 600.
inline bool FineGridVisible(const GridLayout& grid, float pixelsPerMm, float lineScale) {
    const float scale = std::max(lineScale, 0.25f);
    if (grid.fineStepMm * pixelsPerMm < 6.0f * scale) return false;
    const int linesX = (grid.lowX + grid.highX) * 10;
    const int linesY = (grid.lowY + grid.highY) * 10;
    return linesX <= 600 && linesY <= 600;
}

// ---------------------------------------------------------------------------------------------
// Render options
// ---------------------------------------------------------------------------------------------

struct RenderOptions {
    bool showGrid = true;       // F2 measurement grid (grid on the z = 0 plane)
    bool fineGrid = true;       // allow 1 mm lines when zoomed in (false for thumbnails)
    bool showBBox = false;      // F4 bounding box wireframe
    bool showPlate = false;     // F3 K1 Max plate outline (kPlateSizeMm, centred under the model)
    bool overhang = false;      // F7 highlight overhanging (support-needing) faces
    bool wireframe = false;     // draw triangle edges over the solid model
    bool showEdges = true;      // E feature (sharp) edge lines (Mesh::featureEdges, CAD "shaded with edges")
    // Line width multiplier: DPI scale (dpi/96) in the preview, 2.0 for the 2× supersampled thumbnail.
    float lineScale = 1.0f;
    // true: the fit does not depend on the view direction (sphere around the pivot), so the scale does not change
    // while orbiting. On in the preview; off for thumbnails (single frame, tight fit).
    bool stableFit = false;
};

// Options for thumbnails (cx: requested edge, pixels). Thumbnails are drawn at 2× and downscaled by half.
inline RenderOptions ThumbnailRenderOptions(unsigned cx) {
    RenderOptions options;
    options.showGrid = cx >= 96;   // the grid is noise on very small icons
    options.fineGrid = false;
    options.showBBox = false;
    options.showPlate = false;
    options.overhang = false;
    options.wireframe = false;
    options.showEdges = true;
    options.lineScale = 2.0f;
    return options;
}

// Summary of the last successful Render() call (for the overlay and mouse interaction).
struct FrameInfo {
    bool valid = false;
    UINT width = 0;
    UINT height = 0;
    // Screen scale: pixels per 1 mm (same everywhere since the projection is orthographic).
    float pixelsPerMm = 1.0f;
    // Grid layout used (filled in even when showGrid is off).
    GridLayout grid;
    // Smallest MAJOR line spacing drawn in this frame, mm (fine lines excluded). Normally grid.stepMm;
    // when lines get too dense on screen only the strong lines remain -> grid.stepMm * grid.strongEvery.
    // 0 = no grid lines drawn in this frame (showGrid off or too dense).
    float drawnStepMm = 0.0f;
    // Were fine (grid.fineStepMm) lines drawn in this frame?
    bool fineGridDrawn = false;
    // Scene -> clip space (DirectXMath row-vector convention: v' = v · M).
    DirectX::XMFLOAT4X4 viewProj{};
    // Unit vector from the orbit centre to the camera (same as ViewDirection).
    DirectX::XMFLOAT3 viewDir{0.0f, -1.0f, 0.0f};
};

// ---------------------------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------------------------
// Threading: a Renderer instance is used from ONE thread at a time (immediate context).
// The thumbnail provider uses a single shared instance under g_gpuMutex. Render() / Blit() set up ALL the
// pipeline state they need (RT, viewport, blend, depth, raster, topology, shader, CB, VB)
// on every call; they do not assume earlier state is kept (Direct2D may run in between).

class Renderer {
public:
    bool Initialize(ID3D11Device* device);
    void Reset();
    // Releases the model GPU buffers, target textures and readback texture (shaders and states stay).
    // The next Render() draws only the background (until SetMesh is called). The thumbnail provider calls it after
    // every request, the preview when switching to a new file.
    void ReleaseResources();

    // The mesh is in scene space (model.h). bounds/radius are taken from it; the fit state is reset.
    bool SetMesh(const Mesh& mesh);
    UINT VertexCount() const { return m_vertexCount; }

    // Draws the scene to its own MSAA target and resolves it to a single-sample B8G8R8A8 texture (alpha 1 everywhere).
    //   samples      : MSAA sample count (1 is used if unsupported)
    //   fitSmoothing : 1 = apply the fit immediately; (0,1) = approach by that fraction each frame (preview 0.15)
    // Returns false on failure (no device / target could not be created).
    bool Render(UINT width, UINT height, const CameraState& camera, const RenderOptions& options,
                UINT samples = 4, float fitSmoothing = 1.0f);

    // Copies the resolved image to the given target (swap chain back buffer).
    void Blit(ID3D11RenderTargetView* target, UINT width, UINT height);

    // Reads the resolved image as BGRA (top to bottom, row = width*4).
    bool ReadPixels(std::vector<uint8_t>& pixels, UINT& width, UINT& height);

    // Resolved (single-sample) texture: DXGI_FORMAT_B8G8R8A8_UNORM, BIND_RENDER_TARGET | BIND_SHADER_RESOURCE.
    // The test tool draws the overlay (Overlay::Draw) directly on it and reads it back with ReadPixels.
    // Returns null before the first successful Render() call. Ownership stays with the Renderer (no AddRef);
    // if a later Render() is called with a different size the texture is re-created (the pointer becomes invalid).
    ID3D11Texture2D* ResolvedTexture() const { return m_targets.resolved.Get(); }

    // Summary of the last successful Render(). valid = false if Render was never called.
    const FrameInfo& LastFrame() const { return m_frame; }

    // Converts a scene point to the pixel coordinates of the last frame (top-left 0,0; +y down).
    // Returns false if there is no frame. Produces coordinates (returns true) even if the point is out of view.
    bool ProjectToScreen(float sceneX, float sceneY, float sceneZ, float& screenX, float& screenY) const;

    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }

private:
    // ---- everything below belongs to the "renderer" unit and may be changed freely ----
    bool CreateDeviceContextPieces();
    bool EnsureTargets(UINT width, UINT height, UINT samples);
    struct FitFrame {
        float centerX = 0.0f;
        float centerY = 0.0f;
        float halfBase = 0.0f;
    };
    FitFrame ComputeFit(const DirectX::XMMATRIX& view, float aspect, bool includePlate, bool includeBBox) const;
    void DrawFeatureEdges(const DirectX::XMFLOAT4X4& viewProj, bool dark, float lineScale, float depthOffsetClip,
                          float alphaScale);
    void ReleaseTargets();

    // Screen-space constant-width line vertex (48 bytes; same layout as kLineShaderSource).
    struct LineVertex {
        DirectX::XMFLOAT3 a;          // this end
        DirectX::XMFLOAT3 b;          // other end
        DirectX::XMFLOAT4 color;      // rgba
        DirectX::XMFLOAT2 sideWidth;  // (side -1/+1, width in pixels)
    };
    struct LineRanges {
        UINT gridStart = 0;   // grid + plate
        UINT gridCount = 0;
        UINT bboxStart = 0;   // bounding box
        UINT bboxCount = 0;
    };
    static void AppendSegment(std::vector<LineVertex>& out, const DirectX::XMFLOAT3& p, const DirectX::XMFLOAT3& q,
                              const DirectX::XMFLOAT4& color, float widthPx);
    // Builds the grid / plate / bounding box lines into m_lineScratch; fills frame.drawnStepMm and
    // frame.fineGridDrawn.
    LineRanges BuildLines(const RenderOptions& options, bool dark, const GridLayout& grid, float zGrid,
                          float pixelsPerMm, FrameInfo& frame);
    void DrawLines(const LineRanges& ranges, const DirectX::XMFLOAT4X4& viewProj, UINT width, UINT height);
    void ClearExtraStages();

    struct Targets {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> color;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> colorRtv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depth;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthDsv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> resolved;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resolvedSrv;
    };

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_modelVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_modelPs;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_layout;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_fullscreenVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_backgroundPs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_blitPs;

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cameraCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_backgroundCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthOn;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthOff;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_lineVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_linePs;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_lineLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_lineCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_lineVb;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthLessEqual;   // LESS_EQUAL, write on (wireframe)
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthReadOnly;    // LESS_EQUAL, write off (lines)
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_wireRasterizer;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendAlpha;

    // Feature edges: LINELIST, position only per vertex (12 bytes), immutable buffer.
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_edgeVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_edgePs;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_edgeLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_edgeCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_edgeBuffer;
    UINT m_edgeVertexCount = 0;
    double m_edgeLengthMm = 0.0;   // total edge length (to fade by on-screen density)

    std::vector<LineVertex> m_lineScratch;
    std::vector<LineVertex> m_lineFine;
    std::vector<LineVertex> m_lineNormal;
    std::vector<LineVertex> m_lineMain;    // strong + edges
    std::vector<LineVertex> m_lineZero;
    std::vector<LineVertex> m_linePlate;
    std::vector<LineVertex> m_lineBox;

    Targets m_targets;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_samples = 0;
    // CheckMultisampleQualityLevels result cache (not queried every frame): m_msaaEffective is used for
    // the sample count in m_msaaQueried.
    UINT m_msaaQueried = 0;
    UINT m_msaaEffective = 1;
    UINT m_vertexCount = 0;
    float m_radius = 1.0f;
    float m_bounds[6]{};

    std::vector<DirectX::XMFLOAT3> m_fitPoints;
    FitFrame m_fit;
    bool m_fitValid = false;

    FrameInfo m_frame;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    UINT m_stagingWidth = 0;
    UINT m_stagingHeight = 0;
};
