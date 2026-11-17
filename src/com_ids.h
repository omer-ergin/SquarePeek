#pragma once

#include <windows.h>
#include <objidl.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <ocidl.h>
#include <oleidl.h>
#include <propsys.h>

extern HMODULE g_moduleHandle;

inline constexpr GUID CLSID_StlThumbnailProvider =
    {0x2e4a0f31, 0x7c5b, 0x4d81, {0x9e, 0x62, 0x1a, 0x0b, 0x3c, 0x4d, 0x5e, 0x6f}};

inline constexpr GUID CLSID_StlPreviewHandler =
    {0x2e4a0f32, 0x7c5b, 0x4d81, {0x9e, 0x62, 0x1a, 0x0b, 0x3c, 0x4d, 0x5e, 0x6f}};

inline constexpr GUID APPID_StlPreviewHandler =
    {0x2e4a0f33, 0x7c5b, 0x4d81, {0x9e, 0x62, 0x1a, 0x0b, 0x3c, 0x4d, 0x5e, 0x6f}};

// OLD "Open with" launcher (no longer built): kept only to delete registrations left by old installs
// (registration.cpp DeleteObsoleteKeys).
inline constexpr GUID CLSID_StlLaunchBroker =
    {0x2e4a0f34, 0x7c5b, 0x4d81, {0x9e, 0x62, 0x1a, 0x0b, 0x3c, 0x4d, 0x5e, 0x6f}};
inline constexpr GUID APPID_StlLaunchBroker =
    {0x2e4a0f35, 0x7c5b, 0x4d81, {0x9e, 0x62, 0x1a, 0x0b, 0x3c, 0x4d, 0x5e, 0x6f}};

inline constexpr wchar_t kThumbnailHandlerGuid[] = L"{e357fccd-a995-4576-b01f-234630154e96}";
inline constexpr wchar_t kPreviewHandlerGuid[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
inline constexpr wchar_t kStlExtension[] = L".stl";

// Extensions the previewer is registered for (the same two CLSIDs serve both).
inline constexpr const wchar_t* kModelExtensions[] = {L".stl", L".3mf"};

// Names shown in the registry (registration.cpp and installer/STLPreview.nsi use the same text).
inline constexpr wchar_t kThumbnailProviderFriendlyName[] = L"SquarePeek STL/3MF Thumbnail Provider";
inline constexpr wchar_t kPreviewHandlerFriendlyName[] = L"SquarePeek STL/3MF Preview Handler";
