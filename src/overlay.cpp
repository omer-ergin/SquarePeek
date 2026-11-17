#include "overlay.h"

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <map>
#include <utility>

#include <wrl/client.h>

#include "com_util.h"
#include "i18n.h"

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------------------------
// Text fields
// ---------------------------------------------------------------------------------------------

// "%15 dolgu" / "15% infill" (from kTypicalInfill).
static std::wstring InfillText() {
    const std::wstring percent = std::to_wstring(std::llround(kTypicalInfill * 100.0));
    return UiTurkish() ? L"%" + percent + L" dolgu" : percent + L"% infill";
}

// Estimated print mass as a ±10% range: "76–93 g" (a single value if both ends round to the same text).
static std::wstring PrintMassRangeText(double grams) {
    if (!(grams >= 0)) return FormatMassTr(grams);
    const std::wstring low = FormatMassTr(grams * 0.9);
    const std::wstring high = FormatMassTr(grams * 1.1);
    if (low == high) return low;
    // If the unit is the same it is written only at the end ("7.1–8.7 g"; "950 g–1.05 kg" stays as is).
    const size_t lowSpace = low.rfind(L' ');
    const size_t highSpace = high.rfind(L' ');
    if (lowSpace != std::wstring::npos && highSpace != std::wstring::npos &&
        low.compare(lowSpace, std::wstring::npos, high, highSpace, std::wstring::npos) == 0)
        return low.substr(0, lowSpace) + L"–" + high;
    return low + L"–" + high;
}

void FillOverlayInfoFromMesh(OverlayInfo& info, const Mesh& mesh, const std::wstring& fileName) {
    info.fileName = fileName;
    info.sizeLine = FormatSizeText(mesh);

    info.trianglesLine = FormatCountTr(mesh.sourceTriangleCount) +
                         L10n(L" üçgen", mesh.sourceTriangleCount == 1 ? L" triangle" : L" triangles");
    if (mesh.decimated) {
        info.trianglesLine += L" (" + FormatCountTr(mesh.triangleCount) +
                              L10n(L" gösteriliyor)", L" shown)");
    }
    if (mesh.format == ModelFormat::ThreeMf && mesh.objectCount > 1) {
        info.trianglesLine += L" · " + FormatCountTr(mesh.objectCount) + L10n(L" nesne", L" objects");
    }

    const double cm3 = MeshVolumeCm3(mesh);
    const bool openMesh = mesh.openEdges > 0;
    if (cm3 >= 0.0 && openMesh && cm3 < 0.005) {
        // Open mesh without volume (e.g. a flat surface): "0.00 cm³" is meaningless, the line is not shown.
        info.volumeLine.clear();
    } else if (cm3 >= 0.0) {
        // Not closed: the volume is approximate too (model.h); the reason is in the "Open edges" line below.
        info.volumeLine = std::wstring(L10n(L"Hacim: ", L"Volume: ")) + (openMesh ? L"≈ " : L"") +
                          FormatDecimalTr(cm3, cm3 < 10.0 ? 2 : 1) + L" cm³ · " + L10n(L"baskı ≈ ", L"print ≈ ") +
                          PrintMassRangeText(MeshTypicalPrintMassGrams(mesh)) + L" (" + InfillText() + L")";
    } else {
        info.volumeLine.clear();
    }

    if (mesh.openEdges > 0) {
        info.warningLine =
            L10n(L"Açık kenar: ", L"Open edges: ") + FormatCountTr(static_cast<uint64_t>(mesh.openEdges)) +
            L10n(L" (ağ kapalı değil)", L" (mesh not closed)");
    } else {
        info.warningLine.clear();
    }

    constexpr double kPlateLimit = static_cast<double>(kPlateSizeMm) + 0.05;
    info.plateFits = mesh.size[0] <= kPlateLimit && mesh.size[1] <= kPlateLimit && mesh.size[2] <= kPlateLimit;
    info.plateLine = std::wstring(L"K1 Max (300 × 300 × 300 mm): ") + (info.plateFits ? L10n(L"sığıyor", L"fits") : L10n(L"SIĞMIYOR", L"DOES NOT FIT"));
}

void FillOverlayInfoFromFrame(OverlayInfo& info, const FrameInfo& frame, const Mesh& mesh) {
    info.labels.clear();
    if (!frame.valid) {
        info.gridStepMm = 0.0f;
        info.gridFineStepMm = 0.0f;
        return;
    }
    info.gridStepMm = frame.drawnStepMm;
    info.gridFineStepMm = frame.fineGridDrawn ? frame.grid.fineStepMm : 0.0f;

    using namespace DirectX;
    const XMMATRIX viewProj = XMLoadFloat4x4(&frame.viewProj);
    const float width = static_cast<float>(frame.width);
    const float height = static_cast<float>(frame.height);
    auto project = [&](float x, float y, float z, float& px, float& py) {
        const XMVECTOR p = XMVector3TransformCoord(XMVectorSet(x, y, z, 1.0f), viewProj);
        px = (XMVectorGetX(p) * 0.5f + 0.5f) * width;
        py = (1.0f - (XMVectorGetY(p) * 0.5f + 0.5f)) * height;
    };

    const float hx = mesh.bounds[3];
    const float hy = mesh.bounds[4];
    const float sz = mesh.bounds[5];
    const XMFLOAT3& d = frame.viewDir;
    const float sx = d.x >= 0.0f ? 1.0f : -1.0f;
    const float sy = d.y >= 0.0f ? 1.0f : -1.0f;

    float cx = 0.0f;
    float cy = 0.0f;
    project(0.0f, 0.0f, sz * 0.5f, cx, cy);

    auto addLabel = [&](float ax, float ay, float az, std::wstring text) {
        OverlayLabel label;
        project(ax, ay, az, label.x, label.y);
        const float dx = label.x - cx;
        const float dy = label.y - cy;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (std::isfinite(length) && length >= 1.0f) {
            label.dirX = dx / length;
            label.dirY = dy / length;
        }
        if (!std::isfinite(label.x) || !std::isfinite(label.y)) return;
        label.text = std::move(text);
        info.labels.push_back(std::move(label));
    };

    constexpr float kAlongView = 0.985f;
    if (std::fabs(d.x) <= kAlongView) addLabel(0.0f, sy * hy, 0.0f, L"X " + FormatDecimalTr(mesh.size[0], 1));
    if (std::fabs(d.y) <= kAlongView) addLabel(sx * hx, 0.0f, 0.0f, L"Y " + FormatDecimalTr(mesh.size[1], 1));
    if (std::fabs(d.z) <= kAlongView)
        addLabel(sx * hx, -sy * hy, sz * 0.5f, L"Z " + FormatDecimalTr(mesh.size[2], 1));
}

// ---------------------------------------------------------------------------------------------
// Overlay
// ---------------------------------------------------------------------------------------------

namespace {

D2D1_COLOR_F Rgba(float r, float g, float b, float a) {
    return D2D1::ColorF(r, g, b, a);
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alphaScale) {
    color.a *= alphaScale;
    return color;
}

struct Palette {
    D2D1_COLOR_F panel;
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
    D2D1_COLOR_F dim;
    D2D1_COLOR_F warning;
    D2D1_COLOR_F ok;
    D2D1_COLOR_F bbox;
};

Palette MakePalette(bool dark) {
    Palette p{};
    if (dark) {
        p.panel = Rgba(0.09f, 0.09f, 0.10f, 0.74f);
        p.border = Rgba(1.0f, 1.0f, 1.0f, 0.14f);
        p.text = Rgba(0.96f, 0.96f, 0.97f, 1.0f);
        p.warning = Rgba(1.0f, 0.72f, 0.35f, 1.0f);
        p.ok = Rgba(0.55f, 0.85f, 0.60f, 1.0f);
        p.bbox = Rgba(0.60f, 0.83f, 1.0f, 1.0f);
    } else {
        p.panel = Rgba(1.0f, 1.0f, 1.0f, 0.80f);
        p.border = Rgba(0.0f, 0.0f, 0.0f, 0.12f);
        p.text = Rgba(0.10f, 0.10f, 0.12f, 1.0f);
        p.warning = Rgba(0.76f, 0.22f, 0.05f, 1.0f);
        p.ok = Rgba(0.10f, 0.50f, 0.22f, 1.0f);
        p.bbox = Rgba(0.06f, 0.28f, 0.68f, 1.0f);
    }
    p.dim = WithAlpha(p.text, 0.62f);
    return p;
}

struct TextBox {
    ComPtr<IDWriteTextLayout> layout;
    float width = 0.0f;
    float height = 0.0f;
};

bool RectsIntersect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

std::wstring GridLegend(float fineStepMm, float stepMm) {
    auto number = [](float v) { return FormatDecimalTr(v, v < 1.0f ? 1 : 0); };
    const std::wstring title = L10n(L"Izgara: ", L"Grid: ");
    if (fineStepMm > 0.0f) return title + number(fineStepMm) + L" mm / " + number(stepMm) + L" mm";
    return title + number(stepMm) + L" mm";
}

// Process-wide shared D2D / DirectWrite state (used under SharedGpuMutex). Each window only holds
// its own D2D device context, brush, target and icon bitmaps; the factories, the D2D device (one per D3D
// device) and the text format cache are shared.
struct SharedText {
    ComPtr<ID2D1Factory1> factory;   // D2D1_FACTORY_TYPE_MULTI_THREADED
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID3D11Device> d3d;        // D3D device the d2dDevice was created on
    ComPtr<ID2D1Device> d2dDevice;
    long users = 0;                  // number of initialized Overlays; the D2D device is released when it reaches 0
    // Text formats: (size*64, weight) -> format.
    std::map<std::pair<int, int>, ComPtr<IDWriteTextFormat>> formats;
    std::map<int, ComPtr<IDWriteTextFormat>> iconFormats;   // size*64 -> format
    int iconFontState = -1;   // -1: not checked, 0: missing, 1: present
    std::wstring iconFamily;
};

SharedText& Shared() {
    static SharedText* shared = new SharedText();   // leaked on purpose (destruction order at process exit)
    return *shared;
}

}  // namespace

struct Overlay::Impl {
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID2D1Bitmap1> targetBitmap;
    ComPtr<ID2D1SolidColorBrush> brush;
    bool registered = false;   // counted in SharedText::users
    // Text formats live in the shared cache (SharedText).
    std::map<std::pair<int, int>, ComPtr<IDWriteTextFormat>>& formats = Shared().formats;
    // "Open with" icons: OverlayIcon::id -> D2D bitmap (null if creation failed; not retried every frame).
    std::map<uint64_t, ComPtr<ID2D1Bitmap>> iconBitmaps;

    void ReleaseTarget() {
        if (dc) dc->SetTarget(nullptr);
        targetBitmap.Reset();
        target.Reset();
        if (dc) dc->Flush();
    }

    // Releases device-bound D2D objects (factories and the D3D device stay; the next Draw rebuilds them).
    // deviceFailed: the D2D device broke (D2DERR_RECREATE_TARGET); the shared D2D device is re-created too.
    void ReleaseDeviceResources(bool deviceFailed = false) {
        ReleaseTarget();
        iconBitmaps.clear();
        brush.Reset();
        dc.Reset();
        SharedText& shared = Shared();
        if (deviceFailed && d2dDevice && shared.d2dDevice == d2dDevice) {
            shared.d2dDevice.Reset();
            shared.d3d.Reset();
        }
        d2dDevice.Reset();
    }

    bool EnsureDeviceResources() {
        if (dc && brush) return true;
        if (!factory || !dxgiDevice || !d3d) return false;
        brush.Reset();
        dc.Reset();
        d2dDevice.Reset();
        // The D2D device is created once per D3D device and shared.
        SharedText& shared = Shared();
        if (!shared.d2dDevice || shared.d3d != d3d) {
            shared.d2dDevice.Reset();
            shared.d3d.Reset();
            if (FAILED(factory->CreateDevice(dxgiDevice.Get(), shared.d2dDevice.GetAddressOf()))) {
                shared.d2dDevice.Reset();
                return false;
            }
            shared.d3d = d3d;
        }
        d2dDevice = shared.d2dDevice;
        if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.GetAddressOf()))) {
            d2dDevice.Reset();
            return false;
        }
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), brush.GetAddressOf()))) {
            dc.Reset();
            d2dDevice.Reset();
            return false;
        }
        return true;
    }

    IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight) {
        if (!dwrite) return nullptr;
        size = std::max(size, 4.0f);
        const std::pair<int, int> key{static_cast<int>(std::lround(size * 64.0f)), static_cast<int>(weight)};
        const auto found = formats.find(key);
        if (found != formats.end()) return found->second.Get();
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, size, L10n(L"tr-TR", L"en-US"),
                                            format.GetAddressOf())))
            return nullptr;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        IDWriteTextFormat* raw = format.Get();
        formats.emplace(key, std::move(format));
        return raw;
    }

    // Single-line text layout. If maxWidth is exceeded it is trimmed at character level with "…".
    bool MakeText(const std::wstring& text, IDWriteTextFormat* format, float maxWidth, TextBox& out) {
        out = TextBox{};
        if (!format || text.empty() || !dwrite) return false;
        maxWidth = std::max(maxWidth, 1.0f);
        if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, maxWidth,
                                            100000.0f, out.layout.GetAddressOf())))
            return false;
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(out.layout.Get(), sign.GetAddressOf()))) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            out.layout->SetTrimming(&trimming, sign.Get());
        }
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(out.layout->GetMetrics(&metrics))) {
            out.layout.Reset();
            return false;
        }
        out.width = std::min(metrics.widthIncludingTrailingWhitespace, maxWidth);
        out.height = metrics.height;
        return true;
    }

    void Fill(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color) {
        brush->SetColor(color);
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    }

    void Stroke(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color, float width) {
        brush->SetColor(color);
        const float inset = width * 0.5f;
        const D2D1_RECT_F inner{rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset};
        dc->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius, radius), brush.Get(), width);
    }

    void Text(const TextBox& box, float x, float y, const D2D1_COLOR_F& color) {
        if (!box.layout) return;
        brush->SetColor(color);
        dc->DrawTextLayout(D2D1::Point2F(x, y), box.layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // D2D bitmap of the icon (from the cache; created when first seen). null if invalid/failed.
    ID2D1Bitmap* IconBitmap(const OverlayIcon* icon) {
        if (!icon || icon->id == 0 || icon->width == 0 || icon->height == 0 || icon->width > 256 ||
            icon->height > 256 || icon->pixels.size() < static_cast<size_t>(icon->width) * icon->height * 4)
            return nullptr;
        const auto found = iconBitmaps.find(icon->id);
        if (found != iconBitmaps.end()) return found->second.Get();
        ComPtr<ID2D1Bitmap> bitmap;
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(icon->width, icon->height), icon->pixels.data(), icon->width * 4,
                                    &properties, bitmap.GetAddressOf())))
            bitmap.Reset();
        ID2D1Bitmap* raw = bitmap.Get();
        iconBitmaps.emplace(icon->id, std::move(bitmap));
        return raw;
    }

    // Font for the gear icon: "Segoe Fluent Icons" (Windows 11), else "Segoe MDL2 Assets" (Windows 10);
    // if neither exists (or U+E713 is missing) null, and the gear is drawn with geometry. The result is cached.
    int& iconFontState = Shared().iconFontState;
    std::wstring& iconFamily = Shared().iconFamily;
    std::map<int, ComPtr<IDWriteTextFormat>>& iconFormats = Shared().iconFormats;

    const wchar_t* IconFamily() {
        if (iconFontState >= 0) return iconFontState > 0 ? iconFamily.c_str() : nullptr;
        iconFontState = 0;
        ComPtr<IDWriteFontCollection> collection;
        if (!dwrite || FAILED(dwrite->GetSystemFontCollection(collection.GetAddressOf(), FALSE))) return nullptr;
        for (const wchar_t* name : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (FAILED(collection->FindFamilyName(name, &index, &exists)) || !exists) continue;
            ComPtr<IDWriteFontFamily> family;
            ComPtr<IDWriteFont> font;
            BOOL has = FALSE;
            if (SUCCEEDED(collection->GetFontFamily(index, family.GetAddressOf())) &&
                SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                       DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf())) &&
                SUCCEEDED(font->HasCharacter(0xE713, &has)) && has) {
                iconFamily = name;
                iconFontState = 1;
                return iconFamily.c_str();
            }
        }
        return nullptr;
    }

    IDWriteTextFormat* IconFormat(float size) {
        const wchar_t* family = IconFamily();
        if (!family) return nullptr;
        const int key = static_cast<int>(std::lround(std::max(size, 4.0f) * 64.0f));
        const auto found = iconFormats.find(key);
        if (found != iconFormats.end()) return found->second.Get();
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, std::max(size, 4.0f), L"",
                                            format.GetAddressOf())))
            return nullptr;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        IDWriteTextFormat* raw = format.Get();
        iconFormats.emplace(key, std::move(format));
        return raw;
    }

    // Rectangle of the gear button at the top right (whole pixels).
    static D2D1_RECT_F GearRect(float S, float W) {
        const float margin = std::round(10.0f * S);
        const float size = std::round(30.0f * S);
        return D2D1_RECT_F{W - margin - size, margin, W - margin, margin + size};
    }

    void DrawGearGlyph(const D2D1_RECT_F& rect, float S, const D2D1_COLOR_F& color, bool forceDrawn);
    void DrawGear(const OverlayInfo& info, const Palette& palette, float S, float W, float H,
                  const D2D1_RECT_F& rect, std::vector<OverlayHitRect>* hits);
    D2D1_RECT_F DrawSettings(const OverlayInfo& info, const Palette& palette, float S, float W, float H,
                             const D2D1_RECT_F& gearRect, std::vector<OverlayHitRect>* hits);

    void DrawContent(const OverlayInfo& info, float W, float H, std::vector<OverlayHitRect>* hits);
    D2D1_RECT_F DrawInfoPanel(const OverlayInfo& info, const Palette& palette, float S, float W, float maxRight,
                              bool compact);
    void DrawLabels(const OverlayInfo& info, const Palette& palette, float S, float W, float H,
                    const D2D1_RECT_F* panelRect, const D2D1_RECT_F* barRect, const D2D1_RECT_F* gearRect);
    D2D1_RECT_F DrawHelp(const OverlayInfo& info, const Palette& palette, float S, float W, float H,
                         const D2D1_RECT_F* panelRect);
    bool DrawOpenWithBar(const OverlayInfo& info, const Palette& palette, float S, float W, float H,
                         const D2D1_RECT_F* panelRect, D2D1_RECT_F& barRect, std::vector<OverlayHitRect>* hits);
    void DrawToast(const OverlayInfo& info, const Palette& palette, float S, float W, float bottom);
};

void Overlay::Impl::DrawContent(const OverlayInfo& info, float W, float H, std::vector<OverlayHitRect>* hits) {
    const float S = std::clamp(std::isfinite(info.dpiScale) ? info.dpiScale : 1.0f, 0.75f, 4.0f);
    const Palette palette = MakePalette(info.darkTheme);

    if (!info.centerMessage.empty()) {
        // Loading / error frame: only a short text in the middle.
        const float maxWidth = std::max(W - 24.0f * S, 1.0f);
        TextBox title;
        TextBox detail;
        MakeText(info.centerMessage, Format(14.0f * S, DWRITE_FONT_WEIGHT_SEMI_BOLD), maxWidth, title);
        if (!info.centerDetail.empty())
            MakeText(info.centerDetail, Format(11.5f * S, DWRITE_FONT_WEIGHT_NORMAL), maxWidth, detail);
        const float gap = detail.layout ? 4.0f * S : 0.0f;
        const float total = title.height + gap + detail.height;
        const float top = std::floor((H - total) * 0.5f);
        Text(title, std::floor((W - title.width) * 0.5f), top, palette.dim);
        Text(detail, std::floor((W - detail.width) * 0.5f), top + title.height + gap, WithAlpha(palette.dim, 0.8f));
        DrawToast(info, palette, S, W, H - 14.0f * S);
        return;
    }

    float toastBottom = H - 14.0f * S;
    const bool nothing = W < 170.0f * S || H < 90.0f * S;
    if (!nothing) {
        const bool compact = H < 190.0f * S || W < 260.0f * S;
        // The gear is always at the top right; the info panel extends up to its left.
        const D2D1_RECT_F gearRect = GearRect(S, W);
        D2D1_RECT_F panelRect{};
        bool hasPanel = false;
        if (info.settings.showInfo) {
            panelRect = DrawInfoPanel(info, palette, S, W, gearRect.left - 6.0f * S, compact);
            hasPanel = panelRect.right > panelRect.left;
        }
        D2D1_RECT_F barRect{};
        const bool hasBar = !info.openWith.empty() &&
                            DrawOpenWithBar(info, palette, S, W, H, hasPanel ? &panelRect : nullptr, barRect, hits);
        if (info.settings.showInfo && info.settings.showBBox)
            DrawLabels(info, palette, S, W, H, hasPanel ? &panelRect : nullptr, hasBar ? &barRect : nullptr,
                       &gearRect);
        // Buttons under the top panel are not clickable (hit testing already goes back to front; this is so the list
        // contains only what is really visible).
        auto dropCovered = [&](const D2D1_RECT_F& cover) {
            if (!hits || cover.right <= cover.left) return;
            hits->erase(std::remove_if(hits->begin(), hits->end(),
                                       [&](const OverlayHitRect& hit) {
                                           const D2D1_RECT_F r{hit.left, hit.top, hit.right, hit.bottom};
                                           return RectsIntersect(r, cover);
                                       }),
                        hits->end());
        };
        if (info.showHelp) dropCovered(DrawHelp(info, palette, S, W, H, hasPanel ? &panelRect : nullptr));
        if (info.showSettings) {
            // DrawSettings clears the ones below before adding its own hit rectangles.
            DrawSettings(info, palette, S, W, H, gearRect, hits);
        }
        dropCovered(gearRect);
        DrawGear(info, palette, S, W, H, gearRect, hits);
        if (hasBar) toastBottom = barRect.top - 8.0f * S;
    }
    DrawToast(info, palette, S, W, toastBottom);
}

void Overlay::Impl::DrawGearGlyph(const D2D1_RECT_F& rect, float S, const D2D1_COLOR_F& color, bool forceDrawn) {
    brush->SetColor(color);
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    if (!forceDrawn) {
        if (IDWriteTextFormat* format = IconFormat(std::round(16.0f * S))) {
            static const wchar_t kGear[] = L"\xE713";
            dc->DrawText(kGear, 1, format, rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            return;
        }
    }
    // No font: 8-tooth gear with a hole in the middle (ALTERNATE fill).
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(path.GetAddressOf())) || FAILED(path->Open(sink.GetAddressOf())))
        return;
    constexpr int kTeeth = 8;
    constexpr float kPi = 3.14159265358979f;
    const float step = 2.0f * kPi / static_cast<float>(kTeeth);
    const float size = std::round(16.0f * S);
    const float outer = size * 0.5f;
    const float root = size * 0.36f;
    const float hole = size * 0.15f;
    auto polar = [&](float radius, float angle) {
        return D2D1::Point2F(cx + radius * std::cos(angle), cy + radius * std::sin(angle));
    };
    sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
    const float start = -kPi * 0.5f;
    sink->BeginFigure(polar(root, start - 0.26f * step), D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 0; i < kTeeth; ++i) {
        const float a = start + static_cast<float>(i) * step;
        sink->AddLine(polar(outer, a - 0.17f * step));
        sink->AddLine(polar(outer, a + 0.17f * step));
        sink->AddLine(polar(root, a + 0.26f * step));
        sink->AddArc(D2D1::ArcSegment(polar(root, a + 0.74f * step), D2D1::SizeF(root, root), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->BeginFigure(D2D1::Point2F(cx + hole, cy), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx - hole, cy), D2D1::SizeF(hole, hole), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + hole, cy), D2D1::SizeF(hole, hole), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (SUCCEEDED(sink->Close())) dc->FillGeometry(path.Get(), brush.Get());
}

void Overlay::Impl::DrawGear(const OverlayInfo& info, const Palette& palette, float S, float W, float H,
                             const D2D1_RECT_F& rect, std::vector<OverlayHitRect>* hits) {
    const bool hovered = info.hover.kind == OverlayHitKind::Gear;
    const bool pressed = hovered && info.pressed.kind == OverlayHitKind::Gear;
    const D2D1_COLOR_F accent = palette.bbox;
    const float radius = 6.0f * S;
    const float stroke = std::max(1.0f, S);
    Fill(rect, radius, palette.panel);
    if (info.showSettings) Fill(rect, radius, WithAlpha(accent, 0.16f));
    if (pressed) {
        Fill(rect, radius, WithAlpha(accent, 0.34f));
    } else if (hovered) {
        Fill(rect, radius, WithAlpha(accent, 0.22f));
    }
    Stroke(rect, radius, (hovered || info.showSettings) ? WithAlpha(accent, 0.6f) : palette.border, stroke);
    DrawGearGlyph(rect, S, palette.text, info.forceDrawnGear);
    if (hits) hits->push_back(OverlayHitRect{rect.left, rect.top, rect.right, rect.bottom, 0, OverlayHitKind::Gear});

    // Tooltip: below the gear, right-aligned (when the panel is closed; when open, the panel is there).
    if (hovered && !info.showSettings) {
        IDWriteTextFormat* body = Format(12.5f * S, DWRITE_FONT_WEIGHT_NORMAL);
        TextBox hintBox;
        if (body && MakeText(L10n(L"Ayarlar", L"Settings"), body, std::max(W - 40.0f * S, 1.0f), hintBox)) {
            const float hintW = std::ceil(hintBox.width + 20.0f * S);
            const float hintH = std::ceil(hintBox.height + 8.0f * S);
            const float left = std::max(0.0f, std::round(rect.right - hintW));
            const float top = std::round(rect.bottom + 5.0f * S);
            if (top + hintH <= H) {
                const D2D1_RECT_F hintRect{left, top, left + hintW, top + hintH};
                Fill(hintRect, hintH * 0.5f, WithAlpha(palette.panel, 1.2f));
                Stroke(hintRect, hintH * 0.5f, palette.border, stroke);
                Text(hintBox, left + 10.0f * S, top + 4.0f * S, palette.text);
            }
        }
    }
}

D2D1_RECT_F Overlay::Impl::DrawSettings(const OverlayInfo& info, const Palette& palette, float S, float W, float H,
                                        const D2D1_RECT_F& gearRect, std::vector<OverlayHitRect>* hits) {
    struct ToggleRow {
        OverlayToggle id;
        const wchar_t* key;
        const wchar_t* label;
        bool on;
    };
    const PreviewSettings& s = info.settings;
    const ToggleRow allToggles[] = {
        {OverlayToggle::Grid, L"G", L10n(L"Izgara (ölçüm tablası)", L"Grid (measuring plate)"), s.showGrid},
        {OverlayToggle::BBox, L"B", L10n(L"Sınır kutusu ve ölçüler", L"Bounding box & dimensions"), s.showBBox},
        {OverlayToggle::Plate, L"P", L10n(L"K1 Max tablası (300 mm)", L"K1 Max build plate (300 mm)"), s.showPlate},
        {OverlayToggle::Overhang, L"O", L10n(L"Askıda yüzeyler", L"Overhangs"), s.overhang},
        {OverlayToggle::Edges, L"E", L10n(L"Kenar çizgileri", L"Edge lines"), s.showEdges},
        {OverlayToggle::Wireframe, L"W", L10n(L"Tel kafes", L"Wireframe"), s.wireframe},
        {OverlayToggle::AutoRotate, L"R", L10n(L"Otomatik döndürme", L"Auto-rotate"), s.autoRotate},
        {OverlayToggle::Info, L"I", L10n(L"Bilgi yazıları", L"Info text"), s.showInfo},
        {OverlayToggle::OpenWithBar, L"A", L10n(L"Birlikte aç çubuğu", L"Open-with bar"), s.showOpenWith},
    };
    constexpr size_t kMaxToggles = sizeof(allToggles) / sizeof(allToggles[0]);
    // "A" only if the file path is known (same rule as the help panel).
    ToggleRow toggles[kMaxToggles] = {};
    size_t toggleCount = 0;
    for (const ToggleRow& row : allToggles) {
        if (row.id == OverlayToggle::OpenWithBar && !info.canOpenWith) continue;
        toggles[toggleCount++] = row;
    }
    const wchar_t* const viewNames[7] = {
        L10n(L"İzo", L"Iso"),   L10n(L"Ön", L"Front"), L10n(L"Arka", L"Back"),    L10n(L"Sol", L"Left"),
        L10n(L"Sağ", L"Right"), L10n(L"Üst", L"Top"),  L10n(L"Alt", L"Bottom"),
    };
    constexpr int kViews = 7;

    struct Built {
        float k = 1.0f;
        float padX = 0, padY = 0, titleGap = 0, rowH = 0, rowGap = 0, switchW = 0, switchH = 0, labelGap = 0;
        float keyGap = 0, sepGap = 0, sectionGap = 0, btnH = 0, btnGap = 0, keyW = 0, innerW = 0;
        int cols = 1, viewRows = 1;
        TextBox title, section, copy, copyKey;
        TextBox labels[kMaxToggles], keys[kMaxToggles];
        TextBox views[kViews];
        float width = 0, height = 0;
        bool ok = false;
    };
    auto build = [&](float factor, Built& b) {
        b = Built{};
        const float k = S * factor;
        b.k = k;
        IDWriteTextFormat* regular = Format(12.5f * k, DWRITE_FONT_WEIGHT_NORMAL);
        IDWriteTextFormat* smallFormat = Format(12.0f * k, DWRITE_FONT_WEIGHT_NORMAL);
        IDWriteTextFormat* titleFormat = Format(14.0f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (!regular || !smallFormat || !titleFormat) return;
        constexpr float kUnbounded = 100000.0f;
        b.padX = std::round(12.0f * k);
        b.padY = std::round(10.0f * k);
        b.titleGap = std::round(6.0f * k);
        b.rowGap = std::round(2.0f * k);
        b.switchW = std::round(28.0f * k);
        b.switchH = std::round(16.0f * k);
        b.labelGap = std::round(9.0f * k);
        b.keyGap = std::round(16.0f * k);
        b.sepGap = std::round(8.0f * k);
        b.sectionGap = std::round(5.0f * k);
        b.btnGap = std::round(4.0f * k);
        if (!MakeText(L10n(L"Ayarlar", L"Settings"), titleFormat, kUnbounded, b.title)) return;
        MakeText(L10n(L"Görünüm (0–6)", L"View (0–6)"), smallFormat, kUnbounded, b.section);
        MakeText(L10n(L"Ölçüyü kopyala", L"Copy size"), smallFormat, kUnbounded, b.copy);
        MakeText(L"C", smallFormat, kUnbounded, b.copyKey);
        float labelW = 0.0f;
        float textH = 0.0f;
        for (size_t i = 0; i < toggleCount; ++i) {
            MakeText(toggles[i].label, regular, kUnbounded, b.labels[i]);
            MakeText(toggles[i].key, regular, kUnbounded, b.keys[i]);
            labelW = std::max(labelW, b.labels[i].width);
            b.keyW = std::max(b.keyW, b.keys[i].width);
            textH = std::max(textH, b.labels[i].height);
        }
        b.rowH = std::ceil(std::max(22.0f * k, textH + 6.0f * k));
        float viewW = 0.0f;
        float viewH = b.copy.height;
        for (int i = 0; i < kViews; ++i) {
            MakeText(viewNames[i], smallFormat, kUnbounded, b.views[i]);
            viewW = std::max(viewW, b.views[i].width);
            viewH = std::max(viewH, b.views[i].height);
        }
        b.btnH = std::ceil(std::max(22.0f * k, viewH + 8.0f * k));
        const float minBtnW = std::ceil(viewW + 14.0f * k);
        const float rowsW = b.switchW + b.labelGap + labelW + b.keyGap + b.keyW;
        const float copyW = b.copy.width + 2.0f * (b.copyKey.width + 18.0f * k);
        // Width so the view buttons fit in at least two rows (4 + 3).
        const float viewsMinW = 4.0f * minBtnW + 3.0f * b.btnGap;
        b.innerW = std::ceil(std::max({b.title.width, rowsW, copyW, viewsMinW, b.section.width}));
        b.cols = std::clamp(static_cast<int>(std::floor((b.innerW + b.btnGap) / (minBtnW + b.btnGap))), 1, kViews);
        b.viewRows = (kViews + b.cols - 1) / b.cols;
        const float rows = static_cast<float>(toggleCount);
        const float viewRows = static_cast<float>(b.viewRows);
        b.width = b.innerW + 2.0f * b.padX;
        b.height = b.padY + b.title.height + b.titleGap + rows * b.rowH + (rows - 1.0f) * b.rowGap + b.sepGap +
                   std::max(1.0f, S) + b.sepGap + b.section.height + b.sectionGap + viewRows * b.btnH +
                   (viewRows - 1.0f) * b.btnGap + 2.0f * b.btnGap + b.btnH + b.padY;
        b.width = std::ceil(b.width);
        b.height = std::ceil(b.height);
        b.ok = true;
    };

    Built built;
    build(1.0f, built);
    if (!built.ok) return D2D1_RECT_F{};

    // Layout: A) below the gear, right-aligned; B) if it does not fit, left of the gear, top-aligned (more
    // vertical room). Scaled down like the help panel if needed (min 0.6); if still too tall, clipped at the bottom.
    const float margin = std::round(10.0f * S);
    const float gap = std::round(6.0f * S);
    const float topA = gearRect.bottom + gap;
    const float availWA = W - 2.0f * margin;
    const float availHA = H - margin - topA;
    const float topB = gearRect.top;
    const float availWB = gearRect.left - gap - margin;
    const float availHB = H - margin - topB;
    auto fit = [&](float aw, float ah) {
        return std::min({1.0f, aw / built.width, ah / built.height});
    };
    const float fitA = fit(availWA, availHA);
    const float fitB = fit(availWB, availHB);
    const bool useB = fitA < 1.0f && fitB > fitA;
    const float availW = useB ? availWB : availWA;
    const float availH = useB ? availHB : availHA;
    float factor = useB ? fitB : fitA;
    if (factor < 1.0f) {
        factor = std::max(factor, 0.6f);
        build(factor, built);
        // Still overflowing because of rounding: shrink a little more.
        while (built.ok && factor > 0.6f && (built.width > availW || built.height > availH)) {
            factor = std::max(0.6f, factor - 0.02f);
            build(factor, built);
        }
        if (!built.ok) return D2D1_RECT_F{};
    }

    const float right = useB ? gearRect.left - gap : W - margin;
    const float left = std::max(0.0f, std::round(right - built.width));
    const float top = useB ? topB : topA;
    const D2D1_RECT_F rect{left, top, left + built.width, top + built.height};

    // "Open with" buttons underneath are no longer clickable (the panel is on top).
    if (hits) {
        hits->erase(std::remove_if(hits->begin(), hits->end(),
                                   [&](const OverlayHitRect& hit) {
                                       const D2D1_RECT_F r{hit.left, hit.top, hit.right, hit.bottom};
                                       return RectsIntersect(r, rect);
                                   }),
                    hits->end());
    }

    const float k = built.k;
    const float radius = 6.0f * S;
    const float stroke = std::max(1.0f, S);
    // Opaque: dimension labels / info panel / bar buttons below must not show through between the rows.
    D2D1_COLOR_F fill = palette.panel;
    fill.a = 1.0f;
    Fill(rect, radius, fill);
    Stroke(rect, radius, palette.border, stroke);
    if (hits) hits->push_back(OverlayHitRect{rect.left, rect.top, rect.right, rect.bottom, 0, OverlayHitKind::Panel});

    const D2D1_COLOR_F accent = palette.bbox;
    const D2D1_COLOR_F subtle = WithAlpha(palette.text, 0.06f);
    auto isHover = [&](OverlayHitKind kind, int index) {
        return info.hover.kind == kind && info.hover.index == index;
    };
    auto isPressed = [&](OverlayHitKind kind, int index) {
        return isHover(kind, index) && info.pressed.kind == kind && info.pressed.index == index;
    };

    const float innerL = left + built.padX;
    const float innerR = left + built.padX + built.innerW;
    float y = top + built.padY;
    Text(built.title, innerL, std::round(y), palette.text);
    y += built.title.height + built.titleGap;

    // On/off rows: [switch] label ........ key
    const D2D1_COLOR_F knobOn = info.darkTheme ? Rgba(0.09f, 0.09f, 0.10f, 1.0f) : Rgba(1.0f, 1.0f, 1.0f, 1.0f);
    for (size_t i = 0; i < toggleCount; ++i) {
        const int id = static_cast<int>(toggles[i].id);
        const D2D1_RECT_F row{std::round(left + built.padX * 0.5f), std::round(y),
                              std::round(innerR + built.padX * 0.5f), std::round(y + built.rowH)};
        if (isPressed(OverlayHitKind::Toggle, id)) {
            Fill(row, 4.0f * k, WithAlpha(accent, 0.24f));
        } else if (isHover(OverlayHitKind::Toggle, id)) {
            Fill(row, 4.0f * k, WithAlpha(palette.text, 0.08f));
        }
        const float midY = (row.top + row.bottom) * 0.5f;
        const D2D1_RECT_F track{innerL, std::round(midY - built.switchH * 0.5f), innerL + built.switchW,
                                std::round(midY - built.switchH * 0.5f) + built.switchH};
        const float trackR = built.switchH * 0.5f;
        const float knobR = trackR - std::max(2.5f * k, 1.5f);
        if (toggles[i].on) {
            Fill(track, trackR, accent);
            brush->SetColor(knobOn);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(track.right - trackR, (track.top + track.bottom) * 0.5f),
                                          knobR, knobR),
                            brush.Get());
        } else {
            Fill(track, trackR, subtle);
            Stroke(track, trackR, WithAlpha(palette.text, 0.55f), std::max(1.0f, 1.2f * k));
            brush->SetColor(WithAlpha(palette.text, 0.62f));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(track.left + trackR, (track.top + track.bottom) * 0.5f),
                                          knobR - std::max(0.5f, 0.5f * k), knobR - std::max(0.5f, 0.5f * k)),
                            brush.Get());
        }
        const TextBox& label = built.labels[i];
        Text(label, innerL + built.switchW + built.labelGap, std::round(midY - label.height * 0.5f), palette.text);
        const TextBox& key = built.keys[i];
        Text(key, std::round(innerR - key.width), std::round(midY - key.height * 0.5f), palette.dim);
        if (hits) hits->push_back(OverlayHitRect{row.left, row.top, row.right, row.bottom, id, OverlayHitKind::Toggle});
        y += built.rowH + built.rowGap;
    }
    y -= built.rowGap;

    // Separator + "View (0–6)"
    y += built.sepGap;
    brush->SetColor(palette.border);
    dc->FillRectangle(D2D1::RectF(innerL, std::round(y), innerR, std::round(y) + std::max(1.0f, std::round(S))),
                      brush.Get());
    y += std::max(1.0f, S) + built.sepGap;
    Text(built.section, innerL, std::round(y), palette.dim);
    y += built.section.height + built.sectionGap;

    // Button drawing (views + copy): same look as the "Open with" buttons.
    auto drawButton = [&](const D2D1_RECT_F& r, OverlayHitKind kind, int index) {
        const float pillR = 5.0f * k;
        const bool hovered = isHover(kind, index);
        Fill(r, pillR, subtle);
        if (isPressed(kind, index)) {
            Fill(r, pillR, WithAlpha(accent, 0.34f));
        } else if (hovered) {
            Fill(r, pillR, WithAlpha(accent, 0.22f));
        }
        Stroke(r, pillR, hovered ? WithAlpha(accent, 0.6f) : palette.border, stroke);
        if (hits) hits->push_back(OverlayHitRect{r.left, r.top, r.right, r.bottom, index, kind});
    };
    const float cellW = (built.innerW - static_cast<float>(built.cols - 1) * built.btnGap) / static_cast<float>(built.cols);
    for (int i = 0; i < kViews; ++i) {
        const int r = i / built.cols;
        const int c = i % built.cols;
        const float x0 = innerL + static_cast<float>(c) * (cellW + built.btnGap);
        const float y0 = y + static_cast<float>(r) * (built.btnH + built.btnGap);
        const D2D1_RECT_F button{std::round(x0), std::round(y0), std::round(x0 + cellW), std::round(y0 + built.btnH)};
        drawButton(button, OverlayHitKind::View, i);
        const TextBox& text = built.views[i];
        Text(text, std::round((button.left + button.right - text.width) * 0.5f),
             std::round((button.top + button.bottom - text.height) * 0.5f), palette.text);
    }
    y += static_cast<float>(built.viewRows) * built.btnH + static_cast<float>(built.viewRows - 1) * built.btnGap +
         2.0f * built.btnGap;

    const D2D1_RECT_F copy{std::round(innerL), std::round(y), std::round(innerR), std::round(y + built.btnH)};
    drawButton(copy, OverlayHitKind::CopySize, 0);
    Text(built.copy, std::round((copy.left + copy.right - built.copy.width) * 0.5f),
         std::round((copy.top + copy.bottom - built.copy.height) * 0.5f), palette.text);
    Text(built.copyKey, std::round(copy.right - 9.0f * k - built.copyKey.width),
         std::round((copy.top + copy.bottom - built.copyKey.height) * 0.5f), palette.dim);
    return rect;
}

bool Overlay::Impl::DrawOpenWithBar(const OverlayInfo& info, const Palette& palette, float S, float W, float H,
                                    const D2D1_RECT_F* panelRect, D2D1_RECT_F& barRect,
                                    std::vector<OverlayHitRect>* hits) {
    constexpr float kUnbounded = 100000.0f;
    const float margin = 10.0f * S;
    const float pad = 4.0f * S;            // container (panel) inner padding
    const float labelPadL = 6.0f * S;      // "Open with:" from the left
    const float labelGap = 8.0f * S;
    const float spacing = 5.0f * S;        // between buttons
    const float iconSize = std::max(std::round(kOpenWithIconDip * S), 8.0f);
    const float pillH = std::ceil(iconSize + 10.0f * S);
    const float avail = W - 2.0f * margin - 2.0f * pad;

    IDWriteTextFormat* body = Format(12.5f * S, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* bold = Format(12.5f * S, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* tagFormat = Format(11.0f * S, DWRITE_FONT_WEIGHT_NORMAL);
    if (!body || !bold || !tagFormat) return false;

    TextBox labelBox;
    MakeText(L10n(L"Birlikte aç:", L"Open with:"), body, kUnbounded, labelBox);

    struct Item {
        int index = -1;
        bool isMore = false;
        ID2D1Bitmap* icon = nullptr;
        TextBox text;
        TextBox suffix;   // "default"
        float width = 0.0f;
    };
    std::vector<Item> items;
    items.reserve(info.openWith.size());
    enum class Mode { FullWithDefaultTag, Full, Compact };
    auto build = [&](Mode mode) {
        items.clear();
        for (size_t i = 0; i < info.openWith.size(); ++i) {
            const OverlayButton& button = info.openWith[i];
            Item item;
            item.index = static_cast<int>(i);
            item.isMore = button.isMore;
            if (button.isMore) {
                MakeText(L"…", bold, kUnbounded, item.text);
                item.width = std::max(pillH, std::ceil(item.text.width + 18.0f * S));
                items.push_back(std::move(item));
                continue;
            }
            item.icon = IconBitmap(button.icon.get());
            if (mode == Mode::Compact) {
                if (item.icon) {
                    item.width = pillH;
                } else {
                    // No icon: the start of the name ("Cre…").
                    MakeText(button.label, body, pillH * 1.6f - 12.0f * S, item.text);
                    item.width = std::max(pillH, std::ceil(item.text.width + 12.0f * S));
                }
            } else {
                MakeText(button.label, button.isDefault ? bold : body, 150.0f * S, item.text);
                if (mode == Mode::FullWithDefaultTag && button.isDefault)
                    MakeText(L10n(L"varsayılan", L"default"), tagFormat, kUnbounded, item.suffix);
                float width = (item.icon ? 7.0f * S + iconSize + 6.0f * S : 10.0f * S) + item.text.width + 10.0f * S;
                if (item.suffix.layout) width += 6.0f * S + item.suffix.width;
                item.width = std::ceil(width);
            }
            items.push_back(std::move(item));
        }
    };
    auto contentWidth = [&](bool withLabel) {
        float width = 0.0f;
        for (const Item& item : items) width += item.width;
        if (!items.empty()) width += spacing * static_cast<float>(items.size() - 1);
        if (withLabel && labelBox.layout) width += labelPadL + labelBox.width + labelGap;
        return width;
    };

    bool showLabel = labelBox.layout != nullptr;
    bool compactMode = false;
    build(Mode::FullWithDefaultTag);
    if (contentWidth(showLabel) > avail) build(Mode::Full);
    if (contentWidth(showLabel) > avail) {
        build(Mode::Compact);
        compactMode = true;
    }
    if (contentWidth(showLabel) > avail) showLabel = false;
    // Still not fitting: trailing program buttons are dropped ("…" stays: everything not listed is reachable there).
    while (contentWidth(showLabel) > avail) {
        auto last = std::find_if(items.rbegin(), items.rend(), [](const Item& item) { return !item.isMore; });
        if (last == items.rend()) break;
        items.erase(std::next(last).base());
    }
    if (items.empty() || contentWidth(showLabel) > avail) return false;

    const float containerW = std::ceil(contentWidth(showLabel) + 2.0f * pad);
    const float containerH = pillH + 2.0f * pad;
    const float bottom = std::floor(H - margin);
    const D2D1_RECT_F container{margin, bottom - containerH, margin + containerW, bottom};
    // In a very short pane, if it overlaps the info panel the bar is not drawn (so texts do not overlap).
    if (container.top < margin || (panelRect && RectsIntersect(container, *panelRect))) return false;

    const float radius = 6.0f * S;
    const float stroke = std::max(1.0f, S);
    Fill(container, radius, palette.panel);
    Stroke(container, radius, palette.border, stroke);

    float x = container.left + pad;
    const float top = container.top + pad;
    if (showLabel) {
        x += labelPadL;
        Text(labelBox, x, std::round(top + (pillH - labelBox.height) * 0.5f), palette.dim);
        x += labelBox.width + labelGap;
    }

    const D2D1_COLOR_F accent = palette.bbox;
    const D2D1_COLOR_F subtle = WithAlpha(palette.text, 0.06f);
    const OverlayButton* hovered = nullptr;
    D2D1_RECT_F hoveredRect{};
    for (const Item& item : items) {
        const OverlayButton& button = info.openWith[static_cast<size_t>(item.index)];
        const D2D1_RECT_F rect{std::round(x), top, std::round(x + item.width), top + pillH};
        const float pillRadius = 5.0f * S;
        Fill(rect, pillRadius, subtle);
        if (button.isDefault) Fill(rect, pillRadius, WithAlpha(accent, 0.14f));
        if (button.pressed) {
            Fill(rect, pillRadius, WithAlpha(accent, 0.34f));
        } else if (button.hovered) {
            Fill(rect, pillRadius, WithAlpha(accent, 0.22f));
        }
        if (button.isDefault) {
            Stroke(rect, pillRadius, WithAlpha(accent, 0.85f), std::max(1.0f, 1.5f * S));
        } else {
            Stroke(rect, pillRadius, button.hovered ? WithAlpha(accent, 0.6f) : palette.border, stroke);
        }

        float cx = rect.left;
        if (item.isMore) {
            Text(item.text, std::round(rect.left + (item.width - item.text.width) * 0.5f),
                 std::round(top + (pillH - item.text.height) * 0.5f), palette.text);
        } else {
            const bool iconOnly = compactMode && item.icon;
            if (item.icon) {
                const float ix = iconOnly ? std::round(rect.left + (item.width - iconSize) * 0.5f)
                                          : std::round(rect.left + 7.0f * S);
                const float iy = std::round(top + (pillH - iconSize) * 0.5f);
                dc->DrawBitmap(item.icon, D2D1::RectF(ix, iy, ix + iconSize, iy + iconSize), 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                cx = ix + iconSize + 6.0f * S;
            } else {
                cx = compactMode ? rect.left + (item.width - item.text.width) * 0.5f : rect.left + 10.0f * S;
            }
            if (!iconOnly && item.text.layout) {
                Text(item.text, std::round(cx), std::round(top + (pillH - item.text.height) * 0.5f), palette.text);
                cx += item.text.width;
            }
            if (item.suffix.layout) {
                Text(item.suffix, std::round(cx + 6.0f * S), std::round(top + (pillH - item.suffix.height) * 0.5f),
                     palette.bbox);
            }
        }

        if (hits) hits->push_back(OverlayHitRect{rect.left, rect.top, rect.right, rect.bottom, item.index});
        if (button.hovered) {
            hovered = &button;
            hoveredRect = rect;
        }
        x += item.width + spacing;
    }
    barRect = container;

    // Description of the hovered button (the only name source in a narrow pane), right above the bar.
    if (hovered) {
        std::wstring hint;
        if (hovered->isMore) {
            hint = L10n(L"Başka bir programla aç…", L"Open with another program…");
        } else {
            hint = UiTurkish() ? hovered->label + L" ile aç" : L"Open with " + hovered->label;
            if (hovered->isDefault) hint += L10n(L" · varsayılan (Enter)", L" · default (Enter)");
        }
        TextBox hintBox;
        if (MakeText(hint, body, std::max(W - 2.0f * margin - 20.0f * S, 1.0f), hintBox)) {
            const float hintW = std::ceil(hintBox.width + 20.0f * S);
            const float hintH = std::ceil(hintBox.height + 8.0f * S);
            float left = std::round(hoveredRect.left + (hoveredRect.right - hoveredRect.left - hintW) * 0.5f);
            left = std::max(margin, std::min(left, W - margin - hintW));
            const float hintTop = container.top - 5.0f * S - hintH;
            if (hintTop >= 0.0f) {
                const D2D1_RECT_F hintRect{left, hintTop, left + hintW, hintTop + hintH};
                Fill(hintRect, hintH * 0.5f, WithAlpha(palette.panel, 1.15f));
                Stroke(hintRect, hintH * 0.5f, palette.border, stroke);
                Text(hintBox, left + 10.0f * S, hintTop + 4.0f * S, palette.text);
                barRect.top = std::min(barRect.top, hintRect.top);
                barRect.right = std::max(barRect.right, hintRect.right);
            }
        }
    }
    return true;
}

// maxRight: the right edge of the panel does not go past this (so it does not cover the gear at the top right).
D2D1_RECT_F Overlay::Impl::DrawInfoPanel(const OverlayInfo& info, const Palette& palette, float S, float W,
                                          float maxRight, bool compact) {
    struct Line {
        TextBox box;
        D2D1_COLOR_F color;
    };
    const float padX = 9.0f * S;
    const float padY = 7.0f * S;
    const float gap = 3.0f * S;
    const float margin = 10.0f * S;
    const float maxPanel = std::max(std::min(W - 20.0f * S, maxRight - margin), 2.0f * padX + 1.0f);
    const float maxInner = std::max(maxPanel - 2.0f * padX, 1.0f);

    IDWriteTextFormat* body = Format(12.5f * S, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* sizeFormat = Format(14.0f * S, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* nameFormat = Format(13.0f * S, DWRITE_FONT_WEIGHT_SEMI_BOLD);

    std::vector<Line> lines;
    lines.reserve(10);
    auto add = [&](const std::wstring& text, IDWriteTextFormat* format, const D2D1_COLOR_F& color) {
        if (text.empty()) return;
        Line line;
        if (!MakeText(text, format, maxInner, line.box)) return;
        line.color = color;
        lines.push_back(std::move(line));
    };

    if (compact) {
        add(info.sizeLine, sizeFormat, palette.text);
        add(info.warningLine, body, palette.warning);
        // The plate warning is shown even with P off (short text in a narrow pane).
        if (!info.plateFits) add(L10n(L"K1 Max: SIĞMIYOR", L"K1 Max: DOES NOT FIT"), body, palette.warning);
    } else {
        add(info.fileName, nameFormat, palette.text);
        add(info.sizeLine, sizeFormat, palette.text);
        add(info.trianglesLine, body, palette.text);
        add(info.volumeLine, body, palette.text);
        add(info.warningLine, body, palette.warning);
        // "fits" only with P on; the "DOES NOT FIT" warning always.
        if (info.settings.showPlate || !info.plateFits)
            add(info.plateLine, body, info.plateFits ? palette.ok : palette.warning);
        if (info.settings.showGrid && info.gridStepMm > 0.0f)
            add(GridLegend(info.gridFineStepMm, info.gridStepMm), body, palette.text);
        if (info.settings.overhang) add(L10n(L"Kırmızı: askıda yüzeyler (45°'den dik)", L"Red: overhangs (steeper than 45°)"), body, palette.warning);
        add(L10n(L"H: yardım", L"H: help"), body, palette.dim);
    }
    if (lines.empty()) return D2D1_RECT_F{};

    float textWidth = 0.0f;
    float textHeight = 0.0f;
    for (const Line& line : lines) {
        textWidth = std::max(textWidth, line.box.width);
        textHeight += line.box.height;
    }
    textHeight += gap * static_cast<float>(lines.size() - 1);

    const float panelWidth = std::min(std::ceil(textWidth + 2.0f * padX), maxPanel);
    const float panelHeight = std::ceil(textHeight + 2.0f * padY);
    const D2D1_RECT_F rect{margin, margin, margin + panelWidth, margin + panelHeight};
    const float radius = 6.0f * S;
    Fill(rect, radius, palette.panel);
    Stroke(rect, radius, palette.border, std::max(1.0f, S));

    float y = margin + padY;
    for (const Line& line : lines) {
        Text(line.box, margin + padX, y, line.color);
        y += line.box.height + gap;
    }
    return rect;
}

void Overlay::Impl::DrawLabels(const OverlayInfo& info, const Palette& palette, float S, float W, float H,
                               const D2D1_RECT_F* panelRect, const D2D1_RECT_F* barRect,
                               const D2D1_RECT_F* gearRect) {
    if (info.labels.empty()) return;
    IDWriteTextFormat* format = Format(12.0f * S, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float padX = 5.0f * S;
    const float padY = 2.0f * S;
    const float edge = 4.0f * S;
    const float offset = 16.0f * S;

    for (const OverlayLabel& label : info.labels) {
        TextBox box;
        if (!MakeText(label.text, format, std::max(W - 2.0f * edge - 2.0f * padX, 1.0f), box)) continue;
        const float pillW = box.width + 2.0f * padX;
        const float pillH = box.height + 2.0f * padY;
        const float centerX = label.x + label.dirX * offset;
        const float centerY = label.y + label.dirY * offset;
        float left = centerX - pillW * 0.5f;
        float top = centerY - pillH * 0.5f;
        left = std::max(edge, std::min(left, W - edge - pillW));
        top = std::max(edge, std::min(top, H - edge - pillH));
        const D2D1_RECT_F rect{left, top, left + pillW, top + pillH};
        if (panelRect && RectsIntersect(rect, *panelRect)) continue;
        if (barRect && RectsIntersect(rect, *barRect)) continue;
        if (gearRect && RectsIntersect(rect, *gearRect)) continue;
        Fill(rect, pillH * 0.5f, palette.panel);
        Text(box, left + padX, top + padY, palette.bbox);
    }
}

D2D1_RECT_F Overlay::Impl::DrawHelp(const OverlayInfo& info, const Palette& palette, float S, float W, float H,
                                    const D2D1_RECT_F* panelRect) {
    struct Row {
        const wchar_t* key;
        const wchar_t* action;
        int state;   // -1: none, 0: off, 1: on
    };
    const PreviewSettings& s = info.settings;
    const Row allRows[] = {
        {L10n(L"Sol sürükle", L"Left drag"), L10n(L"döndür", L"rotate"), -1},
        {L10n(L"Sağ / orta sürükle", L"Right / middle drag"), L10n(L"kaydır", L"pan"), -1},
        {L10n(L"Tekerlek", L"Wheel"), L10n(L"yakınlaştır / uzaklaştır", L"zoom in / out"), -1},
        {L10n(L"Çift tık", L"Double-click"), L10n(L"görünümü sıfırla", L"reset view"), -1},
        {L"0", L10n(L"izometrik görünüm", L"isometric view"), -1},
        {L"1 / 2", L10n(L"ön / arka", L"front / back"), -1},
        {L"3 / 4", L10n(L"sol / sağ", L"left / right"), -1},
        {L"5 / 6", L10n(L"üst / alt", L"top / bottom"), -1},
        {L"G", L10n(L"ızgara (ölçüm tablası)", L"grid (measuring plate)"), s.showGrid ? 1 : 0},
        {L"B", L10n(L"sınır kutusu ve ölçüler", L"bounding box and dimensions"), s.showBBox ? 1 : 0},
        {L"P", L10n(L"K1 Max tablası (300 mm)", L"K1 Max build plate (300 mm)"), s.showPlate ? 1 : 0},
        {L"O", L10n(L"askıda yüzeyler", L"overhangs"), s.overhang ? 1 : 0},
        {L"E", L10n(L"kenar çizgileri", L"edge lines"), s.showEdges ? 1 : 0},
        {L"W", L10n(L"tel kafes", L"wireframe"), s.wireframe ? 1 : 0},
        {L"R", L10n(L"otomatik döndürme", L"auto-rotate"), s.autoRotate ? 1 : 0},
        {L"I", L10n(L"bilgi yazıları", L"info text"), s.showInfo ? 1 : 0},
        {L"A", L10n(L"birlikte aç çubuğu", L"open-with bar"), s.showOpenWith ? 1 : 0},
        {L"Enter", L10n(L"varsayılan programla aç", L"open with default program"), -1},
        {L"C", L10n(L"ölçüyü panoya kopyala", L"copy size to clipboard"), -1},
        {L"⚙", L10n(L"ayarlar (sağ üst, fareyle)", L"settings (top right, mouse)"), -1},
        {L"H", L10n(L"bu yardım", L"this help"), -1},
    };
    constexpr size_t kMaxRows = sizeof(allRows) / sizeof(allRows[0]);
    // "A" / "Enter" only if the file path is known (Enter also needs the bar on); otherwise the key is not ours.
    Row rows[kMaxRows] = {};
    size_t rowCount = 0;
    for (const Row& row : allRows) {
        const bool isA = std::wcscmp(row.key, L"A") == 0;
        const bool isEnter = std::wcscmp(row.key, L"Enter") == 0;
        if ((isA || isEnter) && !info.canOpenWith) continue;
        if (isEnter && !s.showOpenWith) continue;
        rows[rowCount++] = row;
    }

    struct Built {
        TextBox title;
        TextBox keys[kMaxRows];
        TextBox actions[kMaxRows];
        TextBox states[kMaxRows];
        float padX = 0, padY = 0, colGap = 0, rowGap = 0, titleGap = 0;
        float keyW = 0, actionW = 0, stateW = 0, rowH = 0;
        float width = 0, height = 0;
        bool ok = false;
    };

    auto build = [&](float factor, Built& b) {
        b = Built{};
        const float k = S * factor;
        IDWriteTextFormat* regular = Format(12.5f * k, DWRITE_FONT_WEIGHT_NORMAL);
        IDWriteTextFormat* bold = Format(12.5f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        IDWriteTextFormat* titleFormat = Format(14.0f * k, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (!regular || !bold || !titleFormat) return;
        constexpr float kUnbounded = 100000.0f;
        b.padX = 14.0f * k;
        b.padY = 12.0f * k;
        b.colGap = 14.0f * k;
        b.rowGap = 2.0f * k;
        b.titleGap = 6.0f * k;
        if (!MakeText(L10n(L"Kısayollar", L"Shortcuts"), titleFormat, kUnbounded, b.title)) return;
        for (size_t i = 0; i < rowCount; ++i) {
            MakeText(rows[i].key, bold, kUnbounded, b.keys[i]);
            MakeText(rows[i].action, regular, kUnbounded, b.actions[i]);
            if (rows[i].state >= 0)
                MakeText(rows[i].state ? L10n(L"açık", L"on") : L10n(L"kapalı", L"off"), regular, kUnbounded, b.states[i]);
            b.keyW = std::max(b.keyW, b.keys[i].width);
            b.actionW = std::max(b.actionW, b.actions[i].width);
            b.stateW = std::max(b.stateW, b.states[i].width);
            b.rowH = std::max({b.rowH, b.keys[i].height, b.actions[i].height, b.states[i].height});
        }
        const float columns = b.keyW + b.colGap + b.actionW + (b.stateW > 0 ? b.colGap + b.stateW : 0.0f);
        b.width = std::ceil(std::max(columns, b.title.width) + 2.0f * b.padX);
        const float shownRows = static_cast<float>(rowCount);
        b.height = std::ceil(2.0f * b.padY + b.title.height + b.titleGap + shownRows * b.rowH +
                             (shownRows - 1.0f) * b.rowGap);
        b.ok = true;
    };

    Built built;
    build(1.0f, built);
    if (!built.ok) return D2D1_RECT_F{};
    const float limitW = W - 16.0f * S;
    const float limitH = H - 16.0f * S;
    if (built.width > limitW || built.height > limitH) {
        float factor = std::min(limitW / built.width, limitH / built.height);
        factor = std::max(factor, 0.6f);
        build(factor, built);
        if (!built.ok) return D2D1_RECT_F{};
    }

    const float left = std::max(std::floor((W - built.width) * 0.5f), 0.0f);
    const float top = std::max(std::floor((H - built.height) * 0.5f), 0.0f);
    const D2D1_RECT_F rect{left, top, left + built.width, top + built.height};
    const float radius = 6.0f * S;
    // The help panel covers the scene below more; if it overlaps the info panel (small pane) it is drawn fully
    // opaque so the two texts do not overlap and become unreadable.
    D2D1_COLOR_F fill = WithAlpha(palette.panel, 1.2f);
    if (panelRect && RectsIntersect(rect, *panelRect)) fill.a = 1.0f;
    Fill(rect, radius, fill);
    Stroke(rect, radius, palette.border, std::max(1.0f, S));

    float y = top + built.padY;
    Text(built.title, left + built.padX, y, palette.text);
    y += built.title.height + built.titleGap;
    const float keyX = left + built.padX;
    const float actionX = keyX + built.keyW + built.colGap;
    const float stateX = actionX + built.actionW + built.colGap;
    for (size_t i = 0; i < rowCount; ++i) {
        Text(built.keys[i], keyX, y, palette.text);
        Text(built.actions[i], actionX, y, palette.text);
        if (rows[i].state >= 0) Text(built.states[i], stateX, y, rows[i].state ? palette.ok : palette.dim);
        y += built.rowH + built.rowGap;
    }
    return rect;
}

// bottom: bottom edge of the toast (above the bar while the bar is visible).
void Overlay::Impl::DrawToast(const OverlayInfo& info, const Palette& palette, float S, float W, float bottom) {
    if (info.toast.empty()) return;
    const float alpha = std::clamp(std::isfinite(info.toastAlpha) ? info.toastAlpha : 0.0f, 0.0f, 1.0f);
    if (alpha <= 0.0f) return;
    IDWriteTextFormat* format = Format(12.5f * S, DWRITE_FONT_WEIGHT_NORMAL);
    const float padX = 12.0f * S;
    const float padY = 6.0f * S;
    TextBox box;
    if (!MakeText(info.toast, format, std::max(W - 16.0f * S - 2.0f * padX, 1.0f), box)) return;
    const float pillW = std::ceil(box.width + 2.0f * padX);
    const float pillH = std::ceil(box.height + 2.0f * padY);
    const float left = std::floor((W - pillW) * 0.5f);
    const float top = std::max(std::floor(bottom - pillH), 0.0f);
    const D2D1_RECT_F rect{left, top, left + pillW, top + pillH};
    Fill(rect, pillH * 0.5f, WithAlpha(palette.panel, alpha));
    Stroke(rect, pillH * 0.5f, WithAlpha(palette.border, alpha), std::max(1.0f, S));
    Text(box, left + padX, top + padY, WithAlpha(palette.text, alpha));
}

Overlay::Overlay() : m_impl(std::make_unique<Impl>()) {}

Overlay::~Overlay() {
    Reset();
}

bool Overlay::Initialize(ID3D11Device* device) {
    Reset();
    if (!m_impl) return false;
    Impl& impl = *m_impl;
    std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
    SharedText& shared = Shared();
    HRESULT hr = E_INVALIDARG;
    if (device) {
        hr = S_OK;
        if (!shared.factory) {
            D2D1_FACTORY_OPTIONS options{};
            options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
            hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &options,
                                   reinterpret_cast<void**>(shared.factory.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) shared.factory.Reset();
        }
        if (SUCCEEDED(hr) && !shared.dwrite) {
            hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(shared.dwrite.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) shared.dwrite.Reset();
        }
        if (SUCCEEDED(hr)) {
            impl.factory = shared.factory;
            impl.dwrite = shared.dwrite;
            impl.d3d = device;
            hr = device->QueryInterface(IID_PPV_ARGS(impl.dxgiDevice.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            ++shared.users;
            impl.registered = true;
            if (!impl.EnsureDeviceResources()) hr = E_FAIL;
        }
    }
    if (FAILED(hr)) {
        Reset();
        wchar_t message[96] = {};
        _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Overlay initialization failed (0x%08X)",
                     static_cast<unsigned>(hr));
        DebugLog(message);
        return false;
    }
    return true;
}

void Overlay::Reset() {
    if (!m_impl) return;
    Impl& impl = *m_impl;
    std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
    impl.ReleaseDeviceResources();
    impl.dwrite.Reset();
    impl.dxgiDevice.Reset();
    impl.d3d.Reset();
    impl.factory.Reset();
    if (impl.registered) {
        impl.registered = false;
        SharedText& shared = Shared();
        // The D2D device stays even after the last user leaves (cached like the shared D3D device; see renderer.h).
        if (shared.users > 0) --shared.users;
    }
}

void Overlay::ReleaseTarget() {
    if (!m_impl) return;
    std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
    m_impl->ReleaseTarget();
}

bool Overlay::Draw(ID3D11Texture2D* target, const OverlayInfo& info, std::vector<OverlayHitRect>* hits) {
    if (hits) hits->clear();
    if (!m_impl || !target) return false;
    Impl& impl = *m_impl;
    // Immediate context and shared text format cache: all drawing happens under the shared lock.
    std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
    if (!impl.factory || !impl.dwrite || !impl.dxgiDevice) return false;
    if (!impl.EnsureDeviceResources()) return false;

    if (impl.target.Get() != target || !impl.targetBitmap) {
        impl.ReleaseTarget();
        ComPtr<IDXGISurface> surface;
        if (FAILED(target->QueryInterface(IID_PPV_ARGS(surface.GetAddressOf())))) return false;
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        ComPtr<ID2D1Bitmap1> bitmap;
        const HRESULT hr = impl.dc->CreateBitmapFromDxgiSurface(surface.Get(), &properties, bitmap.GetAddressOf());
        if (FAILED(hr)) {
            if (hr == D2DERR_RECREATE_TARGET) impl.ReleaseDeviceResources(true);
            return false;
        }
        impl.target = target;
        impl.targetBitmap = std::move(bitmap);
    }

    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    const float W = static_cast<float>(desc.Width);
    const float H = static_cast<float>(desc.Height);
    if (desc.Width == 0 || desc.Height == 0) return false;

    // The format cache grows as DPI / help scale changes; it is emptied here, not in the MIDDLE of a frame
    // (Format() pointers must stay valid for a whole frame).
    if (impl.formats.size() > 64) impl.formats.clear();
    if (impl.iconFormats.size() > 16) impl.iconFormats.clear();
    if (impl.iconBitmaps.size() > 64) impl.iconBitmaps.clear();

    impl.dc->SetTarget(impl.targetBitmap.Get());
    impl.dc->SetDpi(96.0f, 96.0f);
    impl.dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    impl.dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    impl.dc->BeginDraw();
    impl.dc->SetTransform(D2D1::Matrix3x2F::Identity());
    impl.DrawContent(info, W, H, hits);
    const HRESULT hr = impl.dc->EndDraw();
    if (FAILED(hr)) {
        if (hits) hits->clear();
        impl.ReleaseDeviceResources(hr == D2DERR_RECREATE_TARGET);
        return false;
    }
    return true;
}
