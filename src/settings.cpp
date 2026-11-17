#include "settings.h"

#include <windows.h>

#include "com_util.h"

namespace {

void ReadFlag(const wchar_t* name, bool& value) {
    DWORD data = 0;
    DWORD size = sizeof(data);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kSettingsKeyPath, name, RRF_RT_REG_DWORD, nullptr,
                                        &data, &size);
    if (status == ERROR_SUCCESS && size == sizeof(data)) value = data != 0;
}

bool WriteFlag(HKEY key, const wchar_t* name, bool value) {
    const DWORD data = value ? 1u : 0u;
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data)) ==
           ERROR_SUCCESS;
}

}  // namespace

PreviewSettings LoadPreviewSettings() {
    PreviewSettings settings;
    ReadFlag(kSettingShowGrid, settings.showGrid);
    ReadFlag(kSettingShowBBox, settings.showBBox);
    ReadFlag(kSettingShowPlate, settings.showPlate);
    ReadFlag(kSettingOverhang, settings.overhang);
    ReadFlag(kSettingWireframe, settings.wireframe);
    ReadFlag(kSettingShowEdges, settings.showEdges);
    ReadFlag(kSettingAutoRotate, settings.autoRotate);
    ReadFlag(kSettingShowInfo, settings.showInfo);
    ReadFlag(kSettingShowOpenWith, settings.showOpenWith);
    return settings;
}

void SavePreviewSettings(const PreviewSettings& settings) {
    HKEY key = nullptr;
    const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKeyPath, 0, nullptr,
                                           REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS || !key) {
        DebugLog(L"Could not save settings");
        return;
    }
    bool ok = true;
    ok &= WriteFlag(key, kSettingShowGrid, settings.showGrid);
    ok &= WriteFlag(key, kSettingShowBBox, settings.showBBox);
    ok &= WriteFlag(key, kSettingShowPlate, settings.showPlate);
    ok &= WriteFlag(key, kSettingOverhang, settings.overhang);
    ok &= WriteFlag(key, kSettingWireframe, settings.wireframe);
    ok &= WriteFlag(key, kSettingShowEdges, settings.showEdges);
    ok &= WriteFlag(key, kSettingAutoRotate, settings.autoRotate);
    ok &= WriteFlag(key, kSettingShowInfo, settings.showInfo);
    ok &= WriteFlag(key, kSettingShowOpenWith, settings.showOpenWith);
    RegCloseKey(key);
    if (!ok) DebugLog(L"Could not save settings");
}

void SavePreviewSetting(const wchar_t* name, bool value) {
    if (!name || !*name) return;
    HKEY key = nullptr;
    const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKeyPath, 0, nullptr,
                                           REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS || !key) {
        DebugLog(L"Could not save setting");
        return;
    }
    if (!WriteFlag(key, name, value)) DebugLog(L"Could not save setting");
    RegCloseKey(key);
}
