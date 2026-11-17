#pragma once

// overlay.h — info overlay of the preview pane (F5): draws text with Direct2D + DirectWrite on top of a BGRA D3D11
// texture (swap chain back buffer or Renderer::ResolvedTexture()).
// Owned by the "preview" unit (implementation: overlay.cpp). Also used by the test tool ("overlay" command).
//
// Usage:
//     OverlayInfo info;
//     FillOverlayInfoFromMesh(info, mesh, fileName);                // once, when the model is loaded
//     FillOverlayInfoFromFrame(info, renderer.LastFrame(), mesh);   // after every Render()
//     info.settings = ...; info.darkTheme = ...; info.dpiScale = ...;
//     overlay.Draw(texture, info);
// Swap chain: ALWAYS call ReleaseTarget() / Reset() BEFORE ResizeBuffers and before releasing the device.

#include <d3d11.h>

#include <memory>
#include <string>
#include <vector>

#include "model.h"
#include "renderer.h"
#include "settings.h"

// Short label attached to a point on screen (bounding box dimensions). Coordinates in pixels, top-left 0,0;
// Draw() CENTRES the label 16·dpiScale pixels away from (x, y) in the direction (dirX, dirY)
// (so it does not sit on the edge). dir is a unit vector or (0,0).
struct OverlayLabel {
    float x = 0.0f;
    float y = 0.0f;
    float dirX = 0.0f;
    float dirY = 0.0f;
    std::wstring text;   // e.g. "X 135.8"
};

// Program icon in the "Open with" bar: premultiplied BGRA (WIC 32bppPBGRA), row stride width*4.
// id is unique per icon; Overlay caches the D2D bitmap under this key (released on device loss).
struct OverlayIcon {
    uint64_t id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

// A button in the "Open with" bar (preview pane only; not in thumbnails or the "render" test).
struct OverlayButton {
    std::wstring label;                        // program name ("Creality Print"); ignored if isMore
    std::shared_ptr<const OverlayIcon> icon;   // null: drawn without an icon
    bool isDefault = false;                    // default program for the extension: highlighted, "default"
    bool isMore = false;                       // "…": Windows' "Open with" dialog
    bool hovered = false;
    bool pressed = false;
};

// Kind of overlay item that can be clicked with the mouse.
enum class OverlayHitKind : int {
    None = 0,
    OpenWith,   // "Open with" button; index = OverlayInfo::openWith index
    Gear,       // settings (gear) button at the top right; index = 0
    Toggle,     // on/off row in the settings panel; index = OverlayToggle
    View,       // view button in the settings panel; index = 0..6 (keys 0..6, ViewPreset order)
    CopySize,   // "Copy size" in the settings panel (key C); index = 0
    Panel,      // empty area of the settings panel: swallows the click (no orbit starts), does nothing
};

// On/off rows in the settings panel (keys: G B P O E W R I A).
enum class OverlayToggle : int { Grid, BBox, Plate, Overhang, Edges, Wireframe, AutoRotate, Info, OpenWithBar, Count };

// A hit target (hovered / pressed item). kind == None: none.
struct OverlayTarget {
    OverlayHitKind kind = OverlayHitKind::None;
    int index = -1;
    bool Valid() const { return kind != OverlayHitKind::None; }
    bool operator==(const OverlayTarget& other) const { return kind == other.kind && index == other.index; }
    bool operator!=(const OverlayTarget& other) const { return !(*this == other); }
};

// Rectangle of a clickable item that Draw() actually drew (pixels, top-left 0,0 on the target texture =
// window client area). If kind == OpenWith, button = OverlayInfo::openWith index; for other kinds
// OverlayTarget::index. The list is in draw order: for overlapping rectangles the LATER one is on top (hit
// testing goes from back to front). Buttons that do not fit / lie under the help or settings panel are not listed.
struct OverlayHitRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    int button = -1;
    OverlayHitKind kind = OverlayHitKind::OpenWith;
};

// Edge of the "Open with" icon in DIP (pixels = round(kOpenWithIconDip * dpiScale)).
inline constexpr float kOpenWithIconDip = 16.0f;

struct OverlayInfo {
    // --- model info (filled by FillOverlayInfoFromMesh) ---
    std::wstring fileName;        // "kapak_v3.stl" (may be empty: opened from a stream)
    std::wstring sizeLine;        // "X 135.8 × Y 134.8 × Z 30.0 mm"
    std::wstring trianglesLine;   // "12,345 triangles" | "12,345,678 triangles (4,000,000 shown)"
    std::wstring volumeLine;      // "Volume: 30.0 cm³ · print ≈ 22 g (15% infill)" ; if the mesh is open
                                  // "Volume: ≈ ..." ; empty if unknown, or if open and without volume
    std::wstring warningLine;     // "Open edges: 12 (mesh not closed)" ; empty if none (drawn in the warning colour)
    std::wstring plateLine;       // "K1 Max (300 × 300 × 300 mm): fits" | "...: DOES NOT FIT"
                                  // ("fits" only when P is on, "DOES NOT FIT" is always shown)
    bool plateFits = true;        // sets the plateLine colour (warning colour if it does not fit)

    // --- frame info (filled by FillOverlayInfoFromFrame) ---
    float gridStepMm = 0.0f;      // FrameInfo::drawnStepMm; 0 -> grid line not drawn
    float gridFineStepMm = 0.0f;  // grid.fineStepMm if FrameInfo::fineGridDrawn, else 0 ("Grid: 1 mm / 10 mm")
    // Bounding box dimension labels (drawn only when settings.showBBox and settings.showInfo are on).
    std::vector<OverlayLabel> labels;

    // --- state (filled by the caller) ---
    PreviewSettings settings;     // which lines are shown + on/off marks in the help panel
    bool showHelp = false;        // H: help panel
    bool canOpenWith = false;     // file path known: "A" (and "Enter" if the bar is on) row in the help panel
    std::wstring toast;           // short notification ("Copied: ..."); empty = none
    float toastAlpha = 1.0f;      // 0..1 (fade out)
    bool darkTheme = false;       // IsDarkBackground(camera.background)
    float dpiScale = 1.0f;        // dpi / 96
    // "Open with" bar (bottom left): empty = no bar. Independent of the info text (I). The toast moves above
    // the bar while it is visible. If the pane is narrow only icons are drawn; the name is shown on hover.
    std::vector<OverlayButton> openWith;
    // Settings: the gear button at the top right is always drawn (unless the pane is very small); showSettings
    // opens the panel below it. hover / pressed are only for the Gear / Toggle / View / CopySize / Panel kinds
    // ("Open with" buttons use OverlayButton::hovered / pressed).
    bool showSettings = false;
    OverlayTarget hover;
    OverlayTarget pressed;
    bool forceDrawnGear = false;   // test: the gear is drawn with geometry even if the icon font exists
    // If not empty, ONLY this text is drawn in the middle of the pane (e.g. "Loading…", "This file could not be
    // previewed"); no info panel, gear, bar or hit rectangles. centerDetail: fainter second line below it.
    std::wstring centerMessage;
    std::wstring centerDetail;
};

// Fills the text fields from mesh statistics (fileName, sizeLine, trianglesLine, volumeLine,
// warningLine, plateLine, plateFits). Does not touch other fields.
void FillOverlayInfoFromMesh(OverlayInfo& info, const Mesh& mesh, const std::wstring& fileName);

// Fills gridStepMm / gridFineStepMm and the dimension labels (labels) from the last frame. If frame.valid is
// false, sets gridStepMm = 0 and clears labels. Does not touch other fields.
void FillOverlayInfoFromFrame(OverlayInfo& info, const FrameInfo& frame, const Mesh& mesh);

class Overlay {
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // The device must be created with D3D11_CREATE_DEVICE_BGRA_SUPPORT (CreateD3D11Device does that).
    // Sets up the D2D/DWrite factories and the device context. Returns false on failure; Draw() then draws
    // nothing and returns false (the preview keeps working without text; NOT a fatal error).
    bool Initialize(ID3D11Device* device);

    // Releases all D2D/DWrite/D3D references. Called before the device is released. May be called again.
    void Reset();

    // Releases only the resources tied to the target texture (D2D bitmap + reference held on the texture).
    // MUST be called BEFORE IDXGISwapChain::ResizeBuffers.
    void ReleaseTarget();

    // Draws the overlay on top of the target (alpha blended; the target's own content is kept).
    //   target : DXGI_FORMAT_B8G8R8A8_UNORM, single-sample texture with D3D11_BIND_RENDER_TARGET.
    //            Size is read from the texture. If the same texture is passed repeatedly the D2D target is cached.
    // The texture MUST NOT be bound as an RT to the D3D pipeline during the call (OMSetRenderTargets(0, ...) first).
    // Returns true if drawn. On D2DERR_RECREATE_TARGET etc. it releases internal resources and returns false;
    // they are rebuilt on the next call.
    //   hits   : if not null, cleared and filled with the rectangles of the clickable items drawn ("Open with"
    //            buttons, gear, settings panel) (for mouse hit testing; the last one is on top).
    //            Left empty if drawing fails.
    bool Draw(ID3D11Texture2D* target, const OverlayInfo& info, std::vector<OverlayHitRect>* hits = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
