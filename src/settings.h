#pragma once

// settings.h — preview window settings (F8). Owned by the "preview" unit (implementation: settings.cpp).
// Stored at HKEY_CURRENT_USER\Software\AppDataLow\Software\STLPreview\Settings, one REG_DWORD per field
// (0 / 1). Explorer starts the preview host at low integrity level; under HKCU that level can only write to
// Software\AppDataLow. Value names are the constants below; DO NOT CHANGE THEM.

#include "renderer.h"

inline constexpr wchar_t kSettingsKeyPath[] = L"Software\\AppDataLow\\Software\\STLPreview\\Settings";
inline constexpr wchar_t kSettingShowGrid[] = L"ShowGrid";
inline constexpr wchar_t kSettingShowBBox[] = L"ShowBBox";
inline constexpr wchar_t kSettingShowPlate[] = L"ShowPlate";
inline constexpr wchar_t kSettingOverhang[] = L"ShowOverhang";
inline constexpr wchar_t kSettingWireframe[] = L"Wireframe";
inline constexpr wchar_t kSettingAutoRotate[] = L"AutoRotate";
inline constexpr wchar_t kSettingShowInfo[] = L"ShowInfo";
inline constexpr wchar_t kSettingShowEdges[] = L"ShowEdges";
inline constexpr wchar_t kSettingShowOpenWith[] = L"ShowOpenWith";

struct PreviewSettings {
    bool showGrid = true;      // G
    bool showBBox = true;      // B
    bool showPlate = false;    // P
    bool overhang = false;     // O
    bool wireframe = false;    // W
    bool showEdges = true;     // E
    bool autoRotate = true;    // R
    bool showInfo = true;      // I
    bool showOpenWith = true;  // A: "Open with" bar (preview pane only)
};

// Reads from the registry. If the key or a value is missing / not REG_DWORD, that field keeps its default.
// Never fails, never writes anything.
PreviewSettings LoadPreviewSettings();

// Writes all fields (creates the key if missing). Errors are silently ignored (DebugLog only).
void SavePreviewSettings(const PreviewSettings& settings);

// Writes a SINGLE value only (name: one of the kSetting* constants). The preview uses this so stale values
// don't overwrite other settings changed by another preview window open at the same time.
void SavePreviewSetting(const wchar_t* name, bool value);

// Converts settings to render options. dpiScale = dpi / 96.
inline RenderOptions MakeRenderOptions(const PreviewSettings& settings, float dpiScale) {
    RenderOptions options;
    options.showGrid = settings.showGrid;
    options.fineGrid = true;
    options.showBBox = settings.showBBox;
    options.showPlate = settings.showPlate;
    options.overhang = settings.overhang;
    options.wireframe = settings.wireframe;
    options.showEdges = settings.showEdges;
    options.lineScale = dpiScale > 0.0f ? dpiScale : 1.0f;
    options.stableFit = true;  // don't zoom in and out on its own while rotating
    return options;
}
