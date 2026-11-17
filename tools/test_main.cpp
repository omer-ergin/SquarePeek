// stlprev-test — command-line tool to try the renderer, the overlay, the thumbnail provider and the preview
// handler without Explorer. Output is in English.

#include "com_ids.h"
#include "com_util.h"
#include "model.h"
#include "open_with.h"
#include "overlay.h"
#include "renderer.h"
#include "settings.h"

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

bool RegistrySelfTest(std::wstring& report);   // tools/reg_test.cpp

using Microsoft::WRL::ComPtr;

namespace {
std::wstring g_dllPath;
// --inproc: the registered server (CoCreateInstance) is never tried; the DLL in the build folder is loaded directly.
bool g_inprocOnly = false;

constexpr uint32_t kTestTriangleBudget = 4'000'000;

bool SaveBmp(const wchar_t* path, const std::vector<uint8_t>& bgra, UINT width, UINT height) {
    if (bgra.size() < static_cast<size_t>(width) * height * 4) return false;

    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = static_cast<LONG>(width);
    infoHeader.biHeight = -static_cast<LONG>(height);
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = width * height * 4;
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + infoHeader.biSizeImage;

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) return false;
    fwrite(&fileHeader, 1, sizeof(fileHeader), file);
    fwrite(&infoHeader, 1, sizeof(infoHeader), file);
    fwrite(bgra.data(), 1, static_cast<size_t>(width) * height * 4, file);
    const bool ok = ferror(file) == 0;
    fclose(file);
    return ok;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool CaptureScreen(const RECT& rect, std::vector<uint8_t>& pixels, UINT& width, UINT& height) {
    width = static_cast<UINT>(rect.right - rect.left);
    height = static_cast<UINT>(rect.bottom - rect.top);
    if (width == 0 || height == 0) return false;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ old = SelectObject(memory, dib);
    BitBlt(memory, 0, 0, static_cast<int>(width), static_cast<int>(height), screen, rect.left, rect.top, SRCCOPY);
    pixels.assign(static_cast<uint8_t*>(bits), static_cast<uint8_t*>(bits) + static_cast<size_t>(width) * height * 4);

    SelectObject(memory, old);
    DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return true;
}

bool ReadBitmap(HBITMAP bitmap, std::vector<uint8_t>& pixels, UINT& width, UINT& height) {
    BITMAP source{};
    if (!GetObjectW(bitmap, sizeof(source), &source)) return false;
    width = static_cast<UINT>(source.bmWidth);
    height = static_cast<UINT>(source.bmHeight < 0 ? -source.bmHeight : source.bmHeight);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    HDC screen = GetDC(nullptr);
    const int lines = GetDIBits(screen, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    return lines == static_cast<int>(height);
}

HRESULT CreateComObject(REFCLSID clsid, REFIID riid, void** ppv, bool* usedSurrogate,
                        DWORD context = CLSCTX_ALL) {
    if (usedSurrogate) *usedSurrogate = false;

    HRESULT hr = g_inprocOnly ? REGDB_E_CLASSNOTREG : CoCreateInstance(clsid, nullptr, context, riid, ppv);
    if (SUCCEEDED(hr)) {
        if (usedSurrogate) *usedSurrogate = true;
        return hr;
    }
    {
        wchar_t message[512] = {};
        LPWSTR text = nullptr;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
        _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"CoCreateInstance: 0x%08lX %s",
                     static_cast<unsigned long>(hr), text ? text : L"");
        if (text) LocalFree(text);
        DebugLog(message);
    }

    HMODULE module = LoadLibraryW(g_dllPath.c_str());
    if (!module) return HRESULT_FROM_WIN32(GetLastError());

    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "DllGetClassObject")));
    if (!getClassObject) return E_FAIL;

    ComPtr<IClassFactory> factory;
    hr = getClassObject(clsid, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return hr;
    return factory->CreateInstance(nullptr, riid, ppv);
}

// ---------------------------------------------------------------------------------------------
// Model loading and command-line options
// ---------------------------------------------------------------------------------------------

// Reads the file and parses it with LoadModel. Returns 0 on success, 2 on read error, 3 on parse error
// (the message has been printed). loadMs: LoadModel time only.
int LoadMeshFromFile(const std::wstring& input, Mesh& mesh, uint32_t flags, double* loadMs = nullptr) {
    std::vector<uint8_t> bytes;
    std::wstring error;
    if (!ReadFileAll(input.c_str(), bytes, error)) {
        wprintf(L"Could not read file: %s\r\n", error.c_str());
        return 2;
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const bool loaded = LoadModel(bytes.data(), bytes.size(), input.c_str(), kTestTriangleBudget, mesh, error, flags);
    QueryPerformanceCounter(&end);
    if (loadMs && frequency.QuadPart > 0) {
        *loadMs = 1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) /
                  static_cast<double>(frequency.QuadPart);
    }
    if (!loaded) {
        wprintf(L"Could not load model: %s\r\n", error.c_str());
        return 3;
    }
    return 0;
}

std::wstring MeshSummaryLine(const Mesh& mesh) {
    return L"Triangles: " + FormatCountTr(mesh.sourceTriangleCount) + L", size: " + FormatSizeText(mesh);
}

struct SceneArgs {
    UINT width = 0;
    UINT height = 0;
    CameraState camera;
    RenderOptions options;
    // Overlay (used only by the "overlay" command).
    bool helpPanel = false;
    std::wstring toast;
    float dpiScale = 1.0f;
    // --openwith: draw the "Open with" bar with the real programs that can open the file on this computer.
    bool openWith = false;
    bool openWithAll = false;   // --openwith-all: non-recommended handlers too (layout test only)
    int hoverButton = -1;   // --hover <n>: as if the mouse were over button n
    // Settings (gear) panel.
    bool settingsPanel = false;    // --settings-panel: the panel is drawn open
    OverlayTarget hoverTarget;     // --hover-gear | --hover-toggle <n> | --hover-view <n> | --hover-copy
    bool pressTarget = false;      // --press: the hovered item is drawn pressed
    bool drawnGear = false;        // --drawn-gear: gear drawn with geometry instead of the icon font
    bool noOpenWithPath = false;   // --no-path: as if the file path were unknown (no A row)
    bool showHits = false;         // --show-hits: hit rectangles are outlined in magenta
    PreviewSettings settings;      // --autorotate 0|1, --info 0|1, --openwith-bar 0|1 (the rest come from options)
};

bool ParseFloat(const wchar_t* text, float& value) {
    if (!text || !*text) return false;
    wchar_t* end = nullptr;
    const double parsed = wcstod(text, &end);
    if (!end || *end != L'\0' || !std::isfinite(parsed)) return false;
    value = static_cast<float>(parsed);
    return true;
}

bool ParseUInt(const wchar_t* text, UINT& value) {
    if (!text || !*text) return false;
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(text, &end, 10);
    if (!end || *end != L'\0' || parsed == 0 || parsed > 16384) return false;
    value = static_cast<UINT>(parsed);
    return true;
}

bool ParseBool(const wchar_t* text, bool& value) {
    if (!text) return false;
    if (wcscmp(text, L"1") == 0) {
        value = true;
        return true;
    }
    if (wcscmp(text, L"0") == 0) {
        value = false;
        return true;
    }
    return false;
}

bool ParseViewPreset(const wchar_t* text, ViewPreset& preset) {
    struct Entry {
        const wchar_t* name;
        ViewPreset preset;
    };
    static const Entry entries[] = {
        {L"iso", ViewPreset::Isometric}, {L"front", ViewPreset::Front}, {L"back", ViewPreset::Back},
        {L"left", ViewPreset::Left},     {L"right", ViewPreset::Right}, {L"top", ViewPreset::Top},
        {L"bottom", ViewPreset::Bottom},
    };
    if (!text) return false;
    for (const Entry& entry : entries) {
        if (_wcsicmp(text, entry.name) == 0) {
            preset = entry.preset;
            return true;
        }
    }
    return false;
}

bool IsFlag(const wchar_t* text) { return text && text[0] == L'-' && text[1] == L'-'; }

// Reads the options in argv[first..]. The view preset is applied first; --az/--el/--zoom/--pan
// are written over it regardless of order.
bool ParseSceneFlags(int argc, wchar_t** argv, int first, SceneArgs& args) {
    bool hasPreset = false;
    ViewPreset preset = ViewPreset::Isometric;
    bool hasAz = false, hasEl = false, hasZoom = false, hasPan = false, hasScale = false;
    float az = 0.0f, el = 0.0f, zoom = 1.0f, panX = 0.0f, panY = 0.0f, scale = 1.0f;

    for (int i = first; i < argc; ++i) {
        const std::wstring flag = argv[i];
        auto next = [&](const wchar_t*& value) -> bool {
            if (i + 1 >= argc) return false;
            value = argv[++i];
            return true;
        };
        const wchar_t* value = nullptr;
        bool ok = true;

        if (flag == L"--view") {
            ok = next(value) && ParseViewPreset(value, preset);
            hasPreset = ok;
        } else if (flag == L"--az") {
            ok = next(value) && ParseFloat(value, az);
            hasAz = ok;
        } else if (flag == L"--el") {
            ok = next(value) && ParseFloat(value, el);
            hasEl = ok;
        } else if (flag == L"--zoom") {
            ok = next(value) && ParseFloat(value, zoom);
            hasZoom = ok;
        } else if (flag == L"--pan") {
            const wchar_t* second = nullptr;
            ok = next(value) && next(second) && ParseFloat(value, panX) && ParseFloat(second, panY);
            hasPan = ok;
        } else if (flag == L"--grid") {
            ok = next(value) && ParseBool(value, args.options.showGrid);
        } else if (flag == L"--bbox") {
            ok = next(value) && ParseBool(value, args.options.showBBox);
        } else if (flag == L"--plate") {
            ok = next(value) && ParseBool(value, args.options.showPlate);
        } else if (flag == L"--overhang") {
            ok = next(value) && ParseBool(value, args.options.overhang);
        } else if (flag == L"--wire") {
            ok = next(value) && ParseBool(value, args.options.wireframe);
        } else if (flag == L"--edges") {
            ok = next(value) && ParseBool(value, args.options.showEdges);
        } else if (flag == L"--fine") {
            ok = next(value) && ParseBool(value, args.options.fineGrid);
        } else if (flag == L"--dark") {
            args.camera.background = {32.0f / 255.0f, 32.0f / 255.0f, 34.0f / 255.0f};
        } else if (flag == L"--green") {
            args.camera.background = {0.10f, 0.70f, 0.15f};
        } else if (flag == L"--w") {
            ok = next(value) && ParseUInt(value, args.width);
        } else if (flag == L"--h") {
            ok = next(value) && ParseUInt(value, args.height);
        } else if (flag == L"--scale") {
            ok = next(value) && ParseFloat(value, scale) && scale > 0.0f;
            hasScale = ok;
        } else if (flag == L"--help-panel") {
            args.helpPanel = true;
        } else if (flag == L"--toast") {
            ok = next(value);
            if (ok) args.toast = value;
        } else if (flag == L"--dpi") {
            ok = next(value) && ParseFloat(value, args.dpiScale) && args.dpiScale > 0.0f;
        } else if (flag == L"--openwith-all") {
            args.openWith = true;
            args.openWithAll = true;
        } else if (flag == L"--openwith") {
            args.openWith = true;
        } else if (flag == L"--hover") {
            float hover = 0.0f;
            ok = next(value) && ParseFloat(value, hover) && hover >= 0.0f && hover < 64.0f;
            if (ok) args.hoverButton = static_cast<int>(hover);
        } else if (flag == L"--settings-panel") {
            args.settingsPanel = true;
        } else if (flag == L"--hover-gear") {
            args.hoverTarget = OverlayTarget{OverlayHitKind::Gear, 0};
        } else if (flag == L"--hover-copy") {
            args.hoverTarget = OverlayTarget{OverlayHitKind::CopySize, 0};
        } else if (flag == L"--hover-toggle" || flag == L"--hover-view") {
            UINT index = 0;
            ok = next(value) && (wcscmp(value, L"0") == 0 || ParseUInt(value, index)) && index < 16;
            if (ok)
                args.hoverTarget = OverlayTarget{flag == L"--hover-toggle" ? OverlayHitKind::Toggle : OverlayHitKind::View,
                                                 static_cast<int>(index)};
        } else if (flag == L"--press") {
            args.pressTarget = true;
        } else if (flag == L"--drawn-gear") {
            args.drawnGear = true;
        } else if (flag == L"--no-path") {
            args.noOpenWithPath = true;
        } else if (flag == L"--show-hits") {
            args.showHits = true;
        } else if (flag == L"--autorotate") {
            ok = next(value) && ParseBool(value, args.settings.autoRotate);
        } else if (flag == L"--info") {
            ok = next(value) && ParseBool(value, args.settings.showInfo);
        } else if (flag == L"--openwith-bar") {
            ok = next(value) && ParseBool(value, args.settings.showOpenWith);
        } else {
            wprintf(L"Unknown option: %s\r\n", flag.c_str());
            return false;
        }

        if (!ok) {
            wprintf(L"Invalid or missing value: %s\r\n", flag.c_str());
            return false;
        }
    }

    if (hasPreset) ApplyViewPreset(args.camera, preset);
    if (hasAz) args.camera.azimuthDeg = az;
    if (hasEl) args.camera.elevationDeg = el;
    if (hasZoom) args.camera.zoom = zoom;
    if (hasPan) {
        args.camera.panX = panX;
        args.camera.panY = panY;
    }
    args.options.lineScale = hasScale ? scale : args.dpiScale;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

int RunInfo(const std::wstring& input) {
    Mesh mesh;
    double loadMs = 0.0;
    const int loadResult = LoadMeshFromFile(input, mesh, kModelLoadDefault, &loadMs);
    if (loadResult != 0) return loadResult;

    wprintf(L"File: %s (%s)\r\n", FileNameOf(input).c_str(), ModelFormatName(mesh.format));
    wprintf(L"Size: %s\r\n", FormatSizeText(mesh).c_str());
    wprintf(L"Bounds: min (%s; %s; %s)  max (%s; %s; %s) mm\r\n", FormatDecimalTr(mesh.origMin[0], 3).c_str(),
            FormatDecimalTr(mesh.origMin[1], 3).c_str(), FormatDecimalTr(mesh.origMin[2], 3).c_str(),
            FormatDecimalTr(mesh.origMax[0], 3).c_str(), FormatDecimalTr(mesh.origMax[1], 3).c_str(),
            FormatDecimalTr(mesh.origMax[2], 3).c_str());

    std::wstring triangles = L"Triangles: " + FormatCountTr(mesh.sourceTriangleCount);
    if (mesh.decimated) triangles += L" (shown " + FormatCountTr(mesh.triangleCount) + L")";
    wprintf(L"%s\r\n", triangles.c_str());

    if (mesh.format == ModelFormat::ThreeMf) wprintf(L"Objects: %s\r\n", FormatCountTr(mesh.objectCount).c_str());

    const double volumeCm3 = MeshVolumeCm3(mesh);
    // If the mesh is not closed, volume and mass are approximate (same rule as the overlay).
    const wchar_t* approx = mesh.openEdges > 0 ? L"≈ " : L"";
    if (volumeCm3 >= 0.0) {
        wprintf(L"Volume: %s%s cm³\r\n", approx, FormatDecimalTr(volumeCm3, 2).c_str());
    } else {
        wprintf(L"Volume: unknown\r\n");
    }
    const double areaCm2 = MeshSurfaceAreaCm2(mesh);
    if (areaCm2 >= 0.0) {
        wprintf(L"Surface area: %s cm²\r\n", FormatDecimalTr(areaCm2, 2).c_str());
    } else {
        wprintf(L"Surface area: unknown\r\n");
    }
    const std::wstring density = FormatDecimalTr(kTypicalFilamentDensity, 2);
    wprintf(L"Mass (solid, %s g/cm³): %s%s g\r\n", density.c_str(), approx,
            FormatDecimalTr(MeshSolidMassGrams(mesh), 2).c_str());
    wprintf(L"Mass (typical print): %s%s g  [shell %s mm, %s%% infill, %s g/cm³; support/brim/purge not included]\r\n",
            approx, FormatDecimalTr(MeshTypicalPrintMassGrams(mesh), 2).c_str(),
            FormatDecimalTr(kTypicalShellMm, 2).c_str(), FormatDecimalTr(kTypicalInfill * 100.0, 0).c_str(),
            density.c_str());

    if (mesh.openEdges >= 0) {
        wprintf(L"Open edges: %s\r\n", FormatCountTr(static_cast<uint64_t>(mesh.openEdges)).c_str());
    } else {
        wprintf(L"Open edges: unknown\r\n");
    }
    switch (mesh.featureEdgeStatus) {
        case FeatureEdgeStatus::Ok:
            wprintf(L"Edge lines: %s\r\n", FormatCountTr(mesh.featureEdges.size() / 6).c_str());
            break;
        case FeatureEdgeStatus::TooMany:
            wprintf(L"Edge lines: too many (> %s), not drawn\r\n", FormatCountTr(kFeatureEdgeMaxSegments).c_str());
            break;
        default:
            wprintf(L"Edge lines: not computed\r\n");
            break;
    }

    const uint64_t milliseconds = static_cast<uint64_t>(std::llround(loadMs < 0.0 ? 0.0 : loadMs));
    wprintf(L"Load: %s ms\r\n", FormatCountTr(milliseconds).c_str());
    return 0;
}

// Shared by render and overlay: loads the model, draws it, optionally adds the overlay, saves a BMP.
int RunRenderCommon(const std::wstring& input, const std::wstring& output, SceneArgs& args, bool withOverlay) {
    Mesh mesh;
    const int loadResult = LoadMeshFromFile(input, mesh, kModelLoadDefault);
    if (loadResult != 0) return loadResult;
    wprintf(L"%s\r\n", MeshSummaryLine(mesh).c_str());

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!::CreateD3D11Device(device.GetAddressOf(), context.GetAddressOf())) {
        wprintf(L"No D3D11 device\r\n");
        return 4;
    }

    Renderer renderer;
    if (!renderer.Initialize(device.Get())) {
        wprintf(L"Renderer initialization failed\r\n");
        return 5;
    }
    if (!renderer.SetMesh(mesh)) {
        wprintf(L"Renderer initialization failed\r\n");
        return 6;
    }

    if (!renderer.Render(args.width, args.height, args.camera, args.options, 4)) {
        wprintf(L"Render failed\r\n");
        return 7;
    }

    std::vector<OverlayHitRect> drawnHits;
    if (withOverlay) {
        OverlayInfo info;
        FillOverlayInfoFromMesh(info, mesh, FileNameOf(input));
        FillOverlayInfoFromFrame(info, renderer.LastFrame(), mesh);
        info.settings.showGrid = args.options.showGrid;
        info.settings.showBBox = args.options.showBBox;
        info.settings.showPlate = args.options.showPlate;
        info.settings.overhang = args.options.overhang;
        info.settings.wireframe = args.options.wireframe;
        info.settings.showEdges = args.options.showEdges;
        info.settings.showInfo = args.settings.showInfo;
        info.settings.autoRotate = args.settings.autoRotate;
        info.settings.showOpenWith = args.settings.showOpenWith;
        info.showHelp = args.helpPanel;
        // the tool reads it from the file path (as if the preview knew the path); --no-path as if it did not
        info.canOpenWith = !args.noOpenWithPath;
        info.showSettings = args.settingsPanel;
        info.hover = args.hoverTarget;
        if (args.pressTarget) info.pressed = args.hoverTarget;
        info.forceDrawnGear = args.drawnGear;
        info.toast = args.toast;
        info.toastAlpha = 1.0f;
        info.darkTheme = IsDarkBackground(args.camera.background);
        info.dpiScale = args.dpiScale;

        // "Open with" bar: same layout as the preview window (programs + "…"). No program is launched.
        OpenWithList openWith;
        if (args.openWith && info.canOpenWith && info.settings.showOpenWith) {
            openWith = EnumerateOpenWithApps(PathFindExtensionW(input.c_str()), kOpenWithMaxApps, args.openWithAll);
            const int iconPx = static_cast<int>(std::lround(kOpenWithIconDip * args.dpiScale));
            LoadOpenWithIcons(openWith.apps, iconPx);
            for (size_t i = 0; i <= openWith.apps.size(); ++i) {
                OverlayButton button;
                if (i < openWith.apps.size()) {
                    button.label = openWith.apps[i].uiName;
                    button.icon = openWith.apps[i].icon;
                    button.isDefault = openWith.apps[i].isDefault;
                } else {
                    button.isMore = true;
                }
                button.hovered = static_cast<int>(i) == args.hoverButton;
                info.openWith.push_back(std::move(button));
            }
            wprintf(L"Open with: %zu programs + \"…\" (icon %d px)\r\n", openWith.apps.size(), iconPx);
        }

        Overlay overlay;
        if (!overlay.Initialize(device.Get())) {
            wprintf(L"Overlay initialization failed\r\n");
            return 10;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        std::vector<OverlayHitRect> hits;
        if (!overlay.Draw(renderer.ResolvedTexture(), info, &hits)) {
            wprintf(L"Overlay drawing failed\r\n");
            return 11;
        }
        size_t openWithHits = 0;
        for (const OverlayHitRect& hit : hits) {
            const wchar_t* kind = L"?";
            std::wstring name;
            switch (hit.kind) {
                case OverlayHitKind::OpenWith: {
                    ++openWithHits;
                    kind = L"button";
                    const OverlayButton& button = info.openWith[static_cast<size_t>(hit.button)];
                    name = button.isMore ? L"…" : button.label;
                    break;
                }
                case OverlayHitKind::Gear: kind = L"gear"; break;
                case OverlayHitKind::Toggle: kind = L"toggle"; break;
                case OverlayHitKind::View: kind = L"view"; break;
                case OverlayHitKind::CopySize: kind = L"copy"; break;
                case OverlayHitKind::Panel: kind = L"panel"; break;
                case OverlayHitKind::None: break;
            }
            wprintf(L"  %s %d%s%s%s: %.1f,%.1f - %.1f,%.1f\r\n", kind, hit.button, name.empty() ? L"" : L" [",
                    name.c_str(), name.empty() ? L"" : L"]", hit.left, hit.top, hit.right, hit.bottom);
        }
        if (args.openWith && openWithHits == 0) wprintf(L"  (bar not drawn: no room)\r\n");
        overlay.ReleaseTarget();
        context->Flush();
        drawnHits = std::move(hits);
    }

    std::vector<uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    if (!renderer.ReadPixels(pixels, width, height)) {
        wprintf(L"Could not read image\r\n");
        return 8;
    }
    if (withOverlay && args.showHits) {
        // Pixel bounds of a hit rectangle: [ceil(l-0.5), ceil(r-0.5)) — same as the preview's hit test
        // (pixel centre x+0.5 ∈ [left, right)). Edge pixels are painted magenta.
        auto plot = [&](LONG x, LONG y) {
            if (x < 0 || y < 0 || x >= static_cast<LONG>(width) || y >= static_cast<LONG>(height)) return;
            uint8_t* p = &pixels[(static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4];
            p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255;
        };
        for (const OverlayHitRect& hit : drawnHits) {
            const LONG x0 = static_cast<LONG>(std::ceil(hit.left - 0.5f));
            const LONG x1 = static_cast<LONG>(std::ceil(hit.right - 0.5f)) - 1;
            const LONG y0 = static_cast<LONG>(std::ceil(hit.top - 0.5f));
            const LONG y1 = static_cast<LONG>(std::ceil(hit.bottom - 0.5f)) - 1;
            if (x1 < x0 || y1 < y0) continue;
            for (LONG x = x0; x <= x1; ++x) {
                plot(x, y0);
                plot(x, y1);
            }
            for (LONG y = y0; y <= y1; ++y) {
                plot(x0, y);
                plot(x1, y);
            }
        }
    }

    if (!SaveBmp(output.c_str(), pixels, width, height)) {
        wprintf(L"Could not write %s\r\n", output.c_str());
        return 9;
    }
    wprintf(L"OK: %s (%ux%u)\r\n", output.c_str(), width, height);
    return 0;
}

int RunThumb(const std::wstring& input, const std::wstring& output, UINT size) {
    ComPtr<IThumbnailProvider> provider;
    bool surrogate = false;
    const HRESULT hr = CreateComObject(CLSID_StlThumbnailProvider, IID_PPV_ARGS(provider.GetAddressOf()),
                                       &surrogate);
    if (FAILED(hr)) {
        wprintf(L"Could not create thumbnail provider (CoCreateInstance): 0x%08X\r\n", static_cast<unsigned>(hr));
        return 2;
    }

    ComPtr<IInitializeWithFile> initializer;
    if (FAILED(provider.As(&initializer))) return 3;
    if (FAILED(initializer->Initialize(input.c_str(), STGM_READ))) return 3;

    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    const HRESULT thumbHr = provider->GetThumbnail(size, &bitmap, &alpha);
    if (FAILED(thumbHr) || !bitmap) {
        wprintf(L"GetThumbnail: 0x%08X\r\n", static_cast<unsigned>(thumbHr));
        return 4;
    }

    std::vector<uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    const bool read = ReadBitmap(bitmap, pixels, width, height);
    DeleteObject(bitmap);
    if (!read) return 5;

    if (!SaveBmp(output.c_str(), pixels, width, height)) {
        wprintf(L"Could not write %s\r\n", output.c_str());
        return 6;
    }
    wprintf(L"Thumbnail: %s (%ux%u), alpha=%d\r\n", output.c_str(), width, height, static_cast<int>(alpha));
    return 0;
}

int RunShellThumb(const std::wstring& input, const std::wstring& output, UINT size) {
    ComPtr<IShellItemImageFactory> factory;
    HRESULT hr = SHCreateItemFromParsingName(input.c_str(), nullptr, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        wchar_t message[128] = {};
        _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"SHCreateItemFromParsingName: 0x%08lX",
                     static_cast<unsigned long>(hr));
        DebugLog(message);
        wprintf(L"%s\r\n", message);
        return 2;
    }

    SIZE requested{static_cast<LONG>(size), static_cast<LONG>(size)};
    HBITMAP bitmap = nullptr;
    hr = factory->GetImage(requested, SIIGBF_THUMBNAILONLY | SIIGBF_RESIZETOFIT, &bitmap);
    if (FAILED(hr) || !bitmap) {
        wprintf(L"GetImage: 0x%08lX (could not get thumbnail)\r\n", static_cast<unsigned long>(hr));
        return 3;
    }

    std::vector<uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    const bool read = ReadBitmap(bitmap, pixels, width, height);
    DeleteObject(bitmap);
    if (!read) return 4;
    if (!SaveBmp(output.c_str(), pixels, width, height)) {
        wprintf(L"Could not write %s\r\n", output.c_str());
        return 5;
    }

    wprintf(L"Shell thumbnail: %s (%ux%u)\r\n", output.c_str(), width, height);
    return 0;
}

std::wstring ProcessNameOfWindow(HWND hwnd) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) return L"?";

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return L"pid " + std::to_wstring(processId);

    wchar_t path[MAX_PATH] = {};
    DWORD size = ARRAYSIZE(path);
    std::wstring name = L"pid " + std::to_wstring(processId);
    if (QueryFullProcessImageNameW(process, 0, path, &size)) name = FileNameOf(path) + L" (" + name + L")";
    CloseHandle(process);
    return name;
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// "handlers": prints the programs the "Open with" bar would see. Does NOT LAUNCH any program.
int RunHandlers(const std::wstring& input) {
    const wchar_t* extension = PathFindExtensionW(input.c_str());
    const OpenWithList list = EnumerateOpenWithApps(extension, 64);
    wprintf(L"Extension: %s\r\n", extension);
    wprintf(L"AssocQueryString: exe = %s\r\n",
            list.defaultExecutable.empty() ? L"(none)" : list.defaultExecutable.c_str());
    wprintf(L"                  name = %s\r\n",
            list.defaultFriendlyName.empty() ? L"(none)" : list.defaultFriendlyName.c_str());
    wprintf(L"Filter: %s, handlers: %zu, unique: %zu, default: %s\r\n",
            list.usedRecommendedFilter ? L"ASSOC_FILTER_RECOMMENDED" : L"ASSOC_FILTER_NONE (no recommended)",
            list.totalHandlers, list.apps.size(),
            list.defaultFromAssoc ? L"found via AssocQueryString" : L"not found, first item assumed");
    std::vector<OpenWithApp> apps = list.apps;
    LoadOpenWithIcons(apps, 16);
    for (size_t i = 0; i < apps.size(); ++i) {
        const OpenWithApp& app = apps[i];
        wprintf(L"%2zu. %s%s%s\r\n", i + 1, app.uiName.c_str(), app.isDefault ? L"  [default]" : L"",
                i >= kOpenWithMaxApps ? L"  (not in bar: via \"…\")" : L"");
        wprintf(L"    recommended: %s, name: %s\r\n", app.recommended ? L"yes" : L"no", app.name.c_str());
        wprintf(L"    icon: %s,%d -> %s\r\n", app.iconPath.empty() ? L"(none)" : app.iconPath.c_str(), app.iconIndex,
                app.icon ? L"16 px OK" : L"EXTRACTION FAILED");
    }
    wprintf(L"Bar: at most %zu programs + \"…\" (Windows' Open with dialog)\r\n", kOpenWithMaxApps);

    // Where can a stream-initialized preview find the path? (The handler no longer takes streams; for info.)
    {
        ComPtr<IShellItem> item;
        ComPtr<IStream> stream;
        HRESULT hr = SHCreateItemFromParsingName(input.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = item->BindToHandler(nullptr, BHID_Stream, IID_PPV_ARGS(stream.GetAddressOf()));
        if (SUCCEEDED(hr) && stream) {
            STATSTG stat{};
            if (SUCCEEDED(stream->Stat(&stat, STATFLAG_DEFAULT))) {
                wprintf(L"Stream Stat name: %s\r\n", stat.pwcsName ? stat.pwcsName : L"(none)");
                if (stat.pwcsName) CoTaskMemFree(stat.pwcsName);
            }
            ComPtr<IPersistFile> persistFile;
            LPOLESTR current = nullptr;
            if (SUCCEEDED(stream.As(&persistFile)) && SUCCEEDED(persistFile->GetCurFile(&current)) && current) {
                wprintf(L"Stream IPersistFile::GetCurFile: %s\r\n", current);
                CoTaskMemFree(current);
            } else {
                wprintf(L"Stream IPersistFile: none\r\n");
            }
        }
    }

    // For info: all handlers including non-recommended ones (not shown in the bar; available in the "…" dialog).
    ComPtr<IEnumAssocHandlers> all;
    if (SUCCEEDED(SHAssocEnumHandlers(extension, ASSOC_FILTER_NONE, all.GetAddressOf())) && all) {
        wprintf(L"With ASSOC_FILTER_NONE:\r\n");
        for (int guard = 0; guard < 256; ++guard) {
            ComPtr<IAssocHandler> handler;
            ULONG fetched = 0;
            if (all->Next(1, handler.GetAddressOf(), &fetched) != S_OK || fetched == 0 || !handler) break;
            LPWSTR name = nullptr;
            if (SUCCEEDED(handler->GetUIName(&name)) && name) {
                wprintf(L"    - %s%s\r\n", name, handler->IsRecommended() == S_OK ? L" (recommended)" : L"");
                CoTaskMemFree(name);
            }
        }
    }
    return 0;
}

struct PreviewArgs {
    int seconds = 3;
    bool localServer = false;
    bool useStream = false;   // --stream: IInitializeWithStream (like Explorer)
    bool useItem = false;     // --item: IInitializeWithItem
    std::wstring output = L"preview.bmp";   // --out <file.bmp>
    // --click <x> <y> | --move <x> <y> | --key <vk>: sent to the preview window in order as MESSAGES
    // (no real mouse/keyboard). x < 0 counts from the right edge. <out>_<n>.bmp is saved after each step.
    struct Step {
        wchar_t type = L'c';
        int x = 0;
        int y = 0;
    };
    std::vector<Step> steps;
};

int RunPreview(const std::wstring& input, const PreviewArgs& previewArgs) {
    const int seconds = previewArgs.seconds;
    const bool localServer = previewArgs.localServer;
    const bool useStream = previewArgs.useStream;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = HostWndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"STLPreviewTestHost";
    RegisterClassExW(&windowClass);

    HWND host = CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, L"STL preview test",
                                WS_OVERLAPPEDWINDOW, 80, 80, 900, 700, nullptr, nullptr,
                                windowClass.hInstance, nullptr);
    if (!host) return 2;
    ShowWindow(host, SW_SHOW);
    UpdateWindow(host);

    ComPtr<IPreviewHandler> handler;
    bool surrogate = false;
    const DWORD context = localServer ? CLSCTX_LOCAL_SERVER : CLSCTX_ALL;
    const HRESULT createHr = CreateComObject(CLSID_StlPreviewHandler, IID_PPV_ARGS(handler.GetAddressOf()),
                                             &surrogate, context);
    if (FAILED(createHr)) {
        wprintf(L"Could not create preview handler (CoCreateInstance): 0x%08X\r\n",
                static_cast<unsigned>(createHr));
        DestroyWindow(host);
        return 3;
    }
    wprintf(L"Activation: %s\r\n", surrogate ? L"via registered server" : L"in-process (DLL)");

    if (useStream) {
        // Like Explorer: the shell item's file stream (BHID_Stream) is passed via IInitializeWithStream; the tool's
        // own reference is released at the end of this block (only the handler keeps the stream).
        ComPtr<IShellItem> item;
        ComPtr<IStream> stream;
        HRESULT hr = SHCreateItemFromParsingName(input.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = item->BindToHandler(nullptr, BHID_Stream, IID_PPV_ARGS(stream.GetAddressOf()));
        ComPtr<IInitializeWithStream> streamInitializer;
        ComPtr<IInitializeWithItem> itemInitializer;
        if (SUCCEEDED(hr) && SUCCEEDED(handler.As(&streamInitializer))) {
            hr = streamInitializer->Initialize(stream.Get(), STGM_READ);
            wprintf(L"Source: stream (IInitializeWithStream): 0x%08X\r\n", static_cast<unsigned>(hr));
        } else if (SUCCEEDED(hr) && SUCCEEDED(handler.As(&itemInitializer))) {
            // Like the shell: if the handler does not expose the stream interface, fall back to the shell item.
            hr = itemInitializer->Initialize(item.Get(), STGM_READ);
            wprintf(L"Source: no IInitializeWithStream, shell item (IInitializeWithItem): 0x%08X\r\n",
                    static_cast<unsigned>(hr));
        } else {
            wprintf(L"Source: stream init failed: 0x%08X\r\n", static_cast<unsigned>(FAILED(hr) ? hr : E_NOINTERFACE));
        }
    } else if (previewArgs.useItem) {
        ComPtr<IShellItem> item;
        HRESULT hr = SHCreateItemFromParsingName(input.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf()));
        ComPtr<IInitializeWithItem> itemInitializer;
        if (SUCCEEDED(hr)) hr = handler.As(&itemInitializer);
        if (SUCCEEDED(hr)) hr = itemInitializer->Initialize(item.Get(), STGM_READ);
        wprintf(L"Source: shell item (IInitializeWithItem): 0x%08X\r\n", static_cast<unsigned>(hr));
    } else {
        ComPtr<IInitializeWithFile> initializer;
        handler.As(&initializer);
        if (initializer) initializer->Initialize(input.c_str(), STGM_READ);
    }

    RECT client{};
    GetClientRect(host, &client);
    if (FAILED(handler->SetWindow(host, &client))) {
        wprintf(L"SetWindow failed\r\n");
        DestroyWindow(host);
        return 4;
    }

    ComPtr<IPreviewHandlerVisuals> visuals;
    if (SUCCEEDED(handler.As(&visuals))) visuals->SetBackgroundColor(RGB(32, 32, 32));

    const HRESULT previewHr = handler->DoPreview();
    wprintf(L"DoPreview: 0x%08X\r\n", static_cast<unsigned>(previewHr));

    HWND pane = nullptr;
    handler->QueryFocus(&pane);
    if (pane) wprintf(L"Process that created the view window: %s\r\n", ProcessNameOfWindow(pane).c_str());
    else wprintf(L"View window not created\r\n");

    DebugLog(L"test: message loop");
    auto pump = [](ULONGLONG milliseconds) {
        const ULONGLONG until = GetTickCount64() + milliseconds;
        while (GetTickCount64() < until) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(10);
        }
    };
    pump(static_cast<ULONGLONG>(seconds > 0 ? seconds : 0) * 1000);
    DebugLog(L"test: message loop done");

    auto capture = [&](const std::wstring& path) {
        RECT hostRect{};
        GetWindowRect(host, &hostRect);
        std::vector<uint8_t> shot;
        UINT shotW = 0;
        UINT shotH = 0;
        if (CaptureScreen(hostRect, shot, shotW, shotH) && SaveBmp(path.c_str(), shot, shotW, shotH))
            wprintf(L"Window capture: %s (%ux%u)\r\n", path.c_str(), shotW, shotH);
    };
    // QueryFocus returns the focused window (may be the host); steps go to the preview window (by class name).
    if (HWND child = FindWindowExW(host, nullptr, L"STLPreviewExplorerPane", nullptr)) pane = child;
    if (pane && !previewArgs.steps.empty()) {
        RECT paneRect{};
        GetClientRect(pane, &paneRect);
        wprintf(L"Pane: %ldx%ld, dpi %u\r\n", paneRect.right, paneRect.bottom, GetDpiForWindow(pane));
        std::wstring base = previewArgs.output;
        if (base.size() > 4 && _wcsicmp(base.c_str() + base.size() - 4, L".bmp") == 0) base.resize(base.size() - 4);
        int index = 0;
        for (const PreviewArgs::Step& step : previewArgs.steps) {
            const int x = step.x < 0 ? paneRect.right + step.x : step.x;
            const LPARAM point = MAKELPARAM(x, step.y);
            if (step.type == L'k') {
                PostMessageW(pane, WM_KEYDOWN, static_cast<WPARAM>(step.x), 1);
                PostMessageW(pane, WM_KEYUP, static_cast<WPARAM>(step.x), static_cast<LPARAM>(0xC0000001u));
                wprintf(L"Step %d: key 0x%02X\r\n", index, step.x);
            } else {
                PostMessageW(pane, WM_MOUSEMOVE, 0, point);
                if (step.type == L'c') {
                    PostMessageW(pane, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    PostMessageW(pane, WM_LBUTTONUP, 0, point);
                }
                wprintf(L"Step %d: %s %d,%d\r\n", index, step.type == L'c' ? L"click" : L"hover", x, step.y);
            }
            // Wait longer than the double-click time (so consecutive steps do not count as a double-click).
            pump(std::max<ULONGLONG>(900, GetDoubleClickTime() + 200));
            capture(base + L"_" + std::to_wstring(index) + L".bmp");
            ++index;
        }
    }
    Sleep(400);

    RECT hostRect{};
    GetWindowRect(host, &hostRect);
    std::vector<uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    if (CaptureScreen(hostRect, pixels, width, height) &&
        SaveBmp(previewArgs.output.c_str(), pixels, width, height)) {
        wprintf(L"Window capture: %s (%ux%u)\r\n", previewArgs.output.c_str(), width, height);
    } else {
        wprintf(L"Could not save window capture\r\n");
    }

    // Summary of the model the handler shows (the same file is parsed again here).
    Mesh mesh;
    std::vector<uint8_t> bytes;
    std::wstring error;
    if (ReadFileAll(input.c_str(), bytes, error) &&
        LoadModel(bytes.data(), bytes.size(), input.c_str(), kTestTriangleBudget, mesh, error,
                  kModelLoadSkipTopology | kModelLoadSkipFeatureEdges)) {
        wprintf(L"%s\r\n", MeshSummaryLine(mesh).c_str());
    }

    DebugLog(L"test: Unload");
    handler->Unload();
    DebugLog(L"test: DestroyWindow");
    DestroyWindow(host);
    DebugLog(L"test: end");
    return SUCCEEDED(previewHr) ? 0 : 5;
}


void PrintUsage() {
    wprintf(L"Usage:\r\n");
    wprintf(L"  stlprev-test selftest               mass estimate and register/unregister self-test\r\n");
    wprintf(L"  stlprev-test info         <model>\r\n");
    wprintf(L"  stlprev-test render       <model> <out.bmp> [size] [options]\r\n");
    wprintf(L"  stlprev-test overlay      <model> <out.bmp> [width height] [options]\r\n");
    wprintf(L"                            [--help-panel] [--toast <text>] [--dpi <scale>]\r\n");
    wprintf(L"                            [--openwith|--openwith-all [--hover <n>]]  \"Open with\" bar\r\n");
    wprintf(L"                            [--settings-panel] panel open   [--drawn-gear] gear without icon font\r\n");
    wprintf(L"                            [--hover-gear | --hover-toggle <0-8> | --hover-view <0-6> | --hover-copy]\r\n");
    wprintf(L"                            [--press] hovered item pressed   [--show-hits] hit outlines\r\n");
    wprintf(L"                            [--no-path] as if there were no file path (no A row, no bar)\r\n");
    wprintf(L"                            [--autorotate 0|1] [--info 0|1] [--openwith-bar 0|1]\r\n");
    wprintf(L"  stlprev-test handlers     <model>   programs that can open the file (none is launched)\r\n");
    wprintf(L"  stlprev-test thumb        <model> <out.bmp> [size] [--inproc]\r\n");
    wprintf(L"  stlprev-test shellthumb   <model> <out.bmp> [size]\r\n");
    wprintf(L"  stlprev-test preview      <model> [seconds] [--stream|--item] [--inproc] [--out <file.bmp>]\r\n");
    wprintf(L"  stlprev-test previewlocal <model> [seconds] [--stream|--item] [--out <file.bmp>]\r\n");
    wprintf(L"                            preview/previewlocal: [--click <x> <y>] [--move <x> <y>] [--key <vk>] ...\r\n");
    wprintf(L"                            sent to the pane in order as messages (no real mouse), x<0: from right\r\n");
    wprintf(L"                            <out>_<n>.bmp after each step\r\n");
    wprintf(L"                            --stream: Explorer-like: IInitializeWithStream or IInitializeWithItem\r\n");
    wprintf(L"                            --item:   initialize with IInitializeWithItem\r\n");
    wprintf(L"                            --inproc: skip the registered server, use the DLL in the build folder\r\n");
    wprintf(L"\r\n");
    wprintf(L"  <model>: .stl or .3mf file.\r\n");
    wprintf(L"\r\n");
    wprintf(L"Options (render / overlay):\r\n");
    wprintf(L"  --view iso|front|back|left|right|top|bottom   view preset\r\n");
    wprintf(L"  --az <degrees>  --el <degrees>  --zoom <z>  --pan <x mm> <y mm>\r\n");
    wprintf(L"  --grid 0|1  --bbox 0|1  --plate 0|1  --overhang 0|1  --wire 0|1  --fine 0|1\r\n");
    wprintf(L"  --edges 0|1   feature (sharp) edge lines (default 1)\r\n");
    wprintf(L"  --dark   dark background       --green   green background (for analyze_bmp.py)\r\n");
    wprintf(L"  --w <pixels>  --h <pixels>     --scale <line width multiplier>\r\n");
    wprintf(L"\r\n");
    wprintf(L"Defaults: render 512 x 512, grid on, bounding box off;\r\n");
    wprintf(L"          overlay 900 x 700, grid and bounding box on.\r\n");
}

// UTF-16 to the console, UTF-8 to a pipe/file (non-ASCII characters show correctly in both cases).
void SetupConsoleOutput() {
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD type = output && output != INVALID_HANDLE_VALUE ? GetFileType(output) : FILE_TYPE_UNKNOWN;
    _setmode(_fileno(stdout), type == FILE_TYPE_CHAR ? _O_U16TEXT : _O_U8TEXT);
}

UINT ParseSizeArgument(const wchar_t* text, UINT fallback) {
    UINT value = 0;
    return ParseUInt(text, value) ? value : fallback;
}
}

int wmain(int argc, wchar_t** argv) {
    SetupConsoleOutput();
    // Run system-DPI aware like prevhost.exe (manifest dpiAware=true): the preview pane is created in the same
    // DPI context as in Explorer.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);

    wchar_t modulePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
    g_dllPath = modulePath;
    const size_t slash = g_dllPath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) g_dllPath = g_dllPath.substr(0, slash + 1) + L"STLPreview.dll";

    if (argc == 2 && std::wstring(argv[1]) == L"selftest") {
        // Mass estimate + register/unregister (in a redirected test registry). 0: all passed.
        std::wstring mass;
        std::wstring registry;
        const bool massOk = ModelMassSelfTest(mass);
        const bool registryOk = RegistrySelfTest(registry);
        wprintf(L"%s%s", mass.c_str(), registry.c_str());
        return massOk && registryOk ? 0 : 1;
    }

    if (argc < 3) {
        PrintUsage();
        return 1;
    }

    const std::wstring command = argv[1];
    const std::wstring input = argv[2];

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comHr)) {
        wprintf(L"CoInitializeEx: 0x%08X\r\n", static_cast<unsigned>(comHr));
        return 1;
    }

    int result = 1;
    if (command == L"info") {
        result = RunInfo(input);
    } else if (command == L"render" || command == L"overlay") {
        const bool overlay = command == L"overlay";
        if (argc < 4 || IsFlag(argv[3])) {
            PrintUsage();
        } else {
            const std::wstring output = argv[3];
            SceneArgs args;
            args.options = RenderOptions{};
            args.options.showGrid = true;
            args.options.showBBox = overlay;
            int next = 4;
            if (overlay) {
                args.width = 900;
                args.height = 700;
                if (argc >= 6 && !IsFlag(argv[4]) && !IsFlag(argv[5])) {
                    args.width = ParseSizeArgument(argv[4], 900);
                    args.height = ParseSizeArgument(argv[5], 700);
                    next = 6;
                }
            } else {
                args.width = 512;
                if (argc >= 5 && !IsFlag(argv[4])) {
                    args.width = ParseSizeArgument(argv[4], 512);
                    next = 5;
                }
                args.height = args.width;
            }
            if (ParseSceneFlags(argc, argv, next, args)) {
                result = RunRenderCommon(input, output, args, overlay);
            }
        }
    } else if (command == L"thumb") {
        for (int i = 3; i < argc; ++i) {
            if (std::wcscmp(argv[i], L"--inproc") == 0) g_inprocOnly = true;
        }
        const std::wstring output = argc >= 4 && !IsFlag(argv[3]) ? argv[3] : L"thumb.bmp";
        const UINT size = argc >= 5 && !IsFlag(argv[4]) ? ParseSizeArgument(argv[4], 256) : 256;
        result = RunThumb(input, output, size);
    } else if (command == L"shellthumb") {
        const std::wstring output = argc >= 4 ? argv[3] : L"shellthumb.bmp";
        const UINT size = argc >= 5 ? ParseSizeArgument(argv[4], 256) : 256;
        result = RunShellThumb(input, output, size);
    } else if (command == L"handlers") {
        result = RunHandlers(input);
    } else if (command == L"preview" || command == L"previewlocal") {
        PreviewArgs previewArgs;
        previewArgs.localServer = command == L"previewlocal";
        for (int i = 3; i < argc; ++i) {
            if (std::wcscmp(argv[i], L"--stream") == 0) previewArgs.useStream = true;
            else if (std::wcscmp(argv[i], L"--item") == 0) previewArgs.useItem = true;
            else if (std::wcscmp(argv[i], L"--inproc") == 0) g_inprocOnly = true;
            else if (std::wcscmp(argv[i], L"--out") == 0 && i + 1 < argc) previewArgs.output = argv[++i];
            else if ((std::wcscmp(argv[i], L"--click") == 0 || std::wcscmp(argv[i], L"--move") == 0) && i + 2 < argc) {
                PreviewArgs::Step step;
                step.type = argv[i][2] == L'c' ? L'c' : L'm';
                step.x = _wtoi(argv[i + 1]);
                step.y = _wtoi(argv[i + 2]);
                i += 2;
                previewArgs.steps.push_back(step);
            } else if (std::wcscmp(argv[i], L"--key") == 0 && i + 1 < argc) {
                PreviewArgs::Step step;
                step.type = L'k';
                step.x = static_cast<int>(wcstol(argv[++i], nullptr, 0));
                previewArgs.steps.push_back(step);
            }
            else previewArgs.seconds = _wtoi(argv[i]);
        }
        if (g_inprocOnly && previewArgs.localServer) {
            wprintf(L"--inproc is only used with \"preview\"\r\n");
        } else {
            result = RunPreview(input, previewArgs);
        }
    } else {
        wprintf(L"Unknown command: %s\r\n", command.c_str());
        PrintUsage();
    }

    CoUninitialize();
    return result;
}
