#include "open_with.h"

#include <shellapi.h>
#include <shlguid.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>

#include "com_util.h"
#include "model.h"

using Microsoft::WRL::ComPtr;

namespace {

std::atomic<uint64_t> g_nextIconId{1};

std::wstring TakeCoTaskString(LPWSTR text) {
    std::wstring result = text ? text : L"";
    if (text) CoTaskMemFree(text);
    return result;
}

std::wstring QueryAssoc(ASSOCSTR what, const wchar_t* extension) {
    wchar_t buffer[2048] = {};
    DWORD size = ARRAYSIZE(buffer);
    if (FAILED(AssocQueryStringW(ASSOCF_NOTRUNCATE | ASSOCF_INIT_IGNOREUNKNOWN, what, extension, nullptr, buffer,
                                 &size)))
        return std::wstring();
    return buffer;
}

bool SameText(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring FileNamePart(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Strips quotes and expands environment variables ("%ProgramFiles%\...").
std::wstring CleanPath(std::wstring path) {
    while (!path.empty() && (path.front() == L'"' || path.front() == L' ')) path.erase(path.begin());
    while (!path.empty() && (path.back() == L'"' || path.back() == L' ')) path.pop_back();
    if (path.find(L'%') != std::wstring::npos) {
        wchar_t expanded[2048] = {};
        const DWORD length = ExpandEnvironmentStringsW(path.c_str(), expanded, ARRAYSIZE(expanded));
        if (length > 0 && length <= ARRAYSIZE(expanded)) path = expanded;
    }
    return path;
}

void Collect(const wchar_t* extension, ASSOC_FILTER filter, std::vector<OpenWithApp>& out) {
    ComPtr<IEnumAssocHandlers> enumerator;
    if (FAILED(SHAssocEnumHandlers(extension, filter, enumerator.GetAddressOf())) || !enumerator) return;
    for (int guard = 0; guard < 256; ++guard) {
        ComPtr<IAssocHandler> handler;
        ULONG fetched = 0;
        if (enumerator->Next(1, handler.GetAddressOf(), &fetched) != S_OK || fetched == 0 || !handler) break;
        OpenWithApp app;
        LPWSTR text = nullptr;
        if (SUCCEEDED(handler->GetUIName(&text))) app.uiName = TakeCoTaskString(text);
        text = nullptr;
        if (SUCCEEDED(handler->GetName(&text))) app.name = TakeCoTaskString(text);
        text = nullptr;
        int index = 0;
        if (SUCCEEDED(handler->GetIconLocation(&text, &index))) {
            app.iconPath = TakeCoTaskString(text);
            app.iconIndex = index;
        }
        if (app.uiName.empty()) app.uiName = FileNamePart(app.name);
        if (app.uiName.empty()) continue;
        app.recommended = handler->IsRecommended() == S_OK;
        app.handler = std::move(handler);
        out.push_back(std::move(app));
    }
}

ComPtr<IWICImagingFactory> CreateWicFactory() {
    ComPtr<IWICImagingFactory> factory;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    return factory;
}

// Converts any WIC source to a size × size premultiplied BGRA icon.
std::shared_ptr<const OverlayIcon> IconFromWicSource(IWICImagingFactory* factory, IWICBitmapSource* source,
                                                     UINT size) {
    if (!factory || !source || size == 0) return nullptr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return nullptr;
    ComPtr<IWICBitmapSource> current = converter;
    UINT width = 0;
    UINT height = 0;
    if (FAILED(current->GetSize(&width, &height)) || width == 0 || height == 0) return nullptr;
    if (width != size || height != size) {
        // Scale premultiplied data (no halo on transparent edges).
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) ||
            FAILED(scaler->Initialize(current.Get(), size, size, WICBitmapInterpolationModeFant)))
            return nullptr;
        current = scaler;
    }
    auto icon = std::make_shared<OverlayIcon>();
    icon->width = size;
    icon->height = size;
    icon->pixels.resize(static_cast<size_t>(size) * size * 4);
    if (FAILED(current->CopyPixels(nullptr, size * 4, static_cast<UINT>(icon->pixels.size()), icon->pixels.data())))
        return nullptr;
    // A fully transparent icon (e.g. broken resource) is not drawn.
    bool anyAlpha = false;
    for (size_t i = 3; i < icon->pixels.size(); i += 4) {
        if (icon->pixels[i] != 0) {
            anyAlpha = true;
            break;
        }
    }
    if (!anyAlpha) return nullptr;
    icon->id = g_nextIconId.fetch_add(1);
    return icon;
}

std::shared_ptr<const OverlayIcon> IconFromHicon(IWICImagingFactory* factory, HICON hicon, UINT size) {
    if (!factory || !hicon) return nullptr;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromHICON(hicon, bitmap.GetAddressOf()))) return nullptr;
    return IconFromWicSource(factory, bitmap.Get(), size);
}

// Classic icon location ("C:\...\app.exe", index or -resource ID).
std::shared_ptr<const OverlayIcon> ExtractClassicIcon(IWICImagingFactory* factory, const std::wstring& path,
                                                      int index, UINT size) {
    if (path.empty()) return nullptr;
    HICON hicon = nullptr;
    if (FAILED(SHDefExtractIconW(path.c_str(), index, 0, &hicon, nullptr, MAKELONG(size, size))) || !hicon) {
        hicon = nullptr;
        UINT id = 0;
        if (PrivateExtractIconsW(path.c_str(), index, static_cast<int>(size), static_cast<int>(size), &hicon, &id, 1,
                                 LR_DEFAULTCOLOR) != 1)
            hicon = nullptr;
    }
    if (!hicon) return nullptr;
    std::shared_ptr<const OverlayIcon> icon = IconFromHicon(factory, hicon, size);
    DestroyIcon(hicon);
    return icon;
}

// Packaged (Microsoft Store) app: "@{Package?ms-resource://...}" -> PNG file for the matching scale.
std::shared_ptr<const OverlayIcon> ExtractPackagedIcon(IWICImagingFactory* factory, const std::wstring& location,
                                                       UINT size) {
    wchar_t file[2048] = {};
    if (FAILED(SHLoadIndirectString(location.c_str(), file, ARRAYSIZE(file), nullptr)) || !file[0]) return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(factory->CreateDecoderFromFilename(file, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
        return nullptr;
    return IconFromWicSource(factory, frame.Get(), size);
}

// Real exe path an app execution alias (IO_REPARSE_TAG_APPEXECLINK) points to; empty if not an alias.
// Reparse data: ULONG version, then NUL-separated strings (package family, AUMID, target exe, ...).
std::wstring AppExecLinkTarget(const std::wstring& path) {
    constexpr ULONG kTagAppExecLink = 0x8000001BUL;
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::wstring();
    std::vector<BYTE> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(file, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(),
                                    static_cast<DWORD>(buffer.size()), &returned, nullptr);
    CloseHandle(file);
    constexpr size_t kHeader = sizeof(ULONG) + 2 * sizeof(USHORT);   // tag, data length, reserved
    if (!ok || returned < kHeader + sizeof(ULONG)) return std::wstring();
    ULONG tag = 0;
    std::memcpy(&tag, buffer.data(), sizeof(tag));
    if (tag != kTagAppExecLink) return std::wstring();
    const size_t first = kHeader + sizeof(ULONG);   // after the version
    const size_t count = (returned - first) / sizeof(wchar_t);
    std::wstring strings(count, L'\0');
    std::memcpy(strings.data(), buffer.data() + first, count * sizeof(wchar_t));
    size_t start = 0;
    for (int index = 0; index < 3 && start < strings.size(); ++index) {
        size_t stop = strings.find(L'\0', start);
        if (stop == std::wstring::npos) stop = strings.size();
        if (index == 2) return strings.substr(start, stop - start);
        start = stop + 1;
    }
    return std::wstring();
}

// Last resort: the shell item's icon (IShellItemImageFactory). App execution aliases (MSIX,
// %LOCALAPPDATA%\Microsoft\WindowsApps\...\x.exe, a 0-byte reparse point) can't be opened by SHDefExtractIcon or
// PrivateExtractIcons; the shell item returns the package's icon.
std::shared_ptr<const OverlayIcon> ExtractShellItemIcon(IWICImagingFactory* factory, const std::wstring& path,
                                                        UINT size) {
    if (!factory || path.empty()) return nullptr;
    ComPtr<IShellItemImageFactory> imageFactory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(imageFactory.GetAddressOf()))))
        return nullptr;
    HBITMAP bitmap = nullptr;
    const SIZE wanted{static_cast<LONG>(size), static_cast<LONG>(size)};
    if (FAILED(imageFactory->GetImage(wanted, SIIGBF_ICONONLY, &bitmap)) || !bitmap) return nullptr;
    std::shared_ptr<const OverlayIcon> icon;
    ComPtr<IWICBitmap> wicBitmap;
    if (SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUsePremultipliedAlpha,
                                                   wicBitmap.GetAddressOf())))
        icon = IconFromWicSource(factory, wicBitmap.Get(), size);
    DeleteObject(bitmap);
    return icon;
}

}  // namespace

OpenWithList EnumerateOpenWithApps(const wchar_t* extension, size_t maxApps, bool allHandlers) {
    OpenWithList list;
    if (!extension || !*extension) return list;
    try {
        std::vector<OpenWithApp> all;
        Collect(extension, allHandlers ? ASSOC_FILTER_NONE : ASSOC_FILTER_RECOMMENDED, all);
        if (allHandlers) list.usedRecommendedFilter = false;
        if (all.empty() && !allHandlers) {
            list.usedRecommendedFilter = false;
            Collect(extension, ASSOC_FILTER_NONE, all);
        }
        list.totalHandlers = all.size();
        if (all.empty()) return list;

        // Default program: match exe path first, then exe name, then display name. If none match, no item is
        // marked default (so the bar's "default" never contradicts the program Enter would open).
        list.defaultExecutable = QueryAssoc(ASSOCSTR_EXECUTABLE, extension);
        list.defaultFriendlyName = QueryAssoc(ASSOCSTR_FRIENDLYAPPNAME, extension);
        size_t defaultIndex = all.size();
        for (size_t i = 0; i < all.size() && defaultIndex == all.size(); ++i)
            if (SameText(CleanPath(all[i].name), list.defaultExecutable)) defaultIndex = i;
        const std::wstring defaultExeName = FileNamePart(list.defaultExecutable);
        for (size_t i = 0; i < all.size() && defaultIndex == all.size(); ++i)
            if (SameText(FileNamePart(CleanPath(all[i].name)), defaultExeName)) defaultIndex = i;
        for (size_t i = 0; i < all.size() && defaultIndex == all.size(); ++i)
            if (SameText(all[i].uiName, list.defaultFriendlyName)) defaultIndex = i;
        list.defaultFromAssoc = defaultIndex < all.size();
        if (list.defaultFromAssoc) all[defaultIndex].isDefault = true;
        if (list.defaultFromAssoc && defaultIndex != 0) std::rotate(all.begin(), all.begin() + static_cast<ptrdiff_t>(defaultIndex),
                                           all.begin() + static_cast<ptrdiff_t>(defaultIndex) + 1);

        // Unique by UI name (the default wins since it is first), at most maxApps.
        for (OpenWithApp& app : all) {
            if (list.apps.size() >= maxApps) break;
            const bool duplicate = std::any_of(list.apps.begin(), list.apps.end(), [&](const OpenWithApp& kept) {
                return SameText(kept.uiName, app.uiName);
            });
            if (!duplicate) list.apps.push_back(std::move(app));
        }
    } catch (...) {
        list.apps.clear();
        DebugLog(L"Open with: could not list programs");
    }
    return list;
}

void LoadOpenWithIcons(std::vector<OpenWithApp>& apps, int sizePx) {
    if (apps.empty() || sizePx <= 0 || sizePx > 256) return;
    try {
        const ComPtr<IWICImagingFactory> factory = CreateWicFactory();
        if (!factory) {
            for (OpenWithApp& app : apps) app.icon.reset();
            return;
        }
        const UINT size = static_cast<UINT>(sizePx);
        for (OpenWithApp& app : apps) {
            app.icon.reset();
            const std::wstring location = CleanPath(app.iconPath);
            if (location.rfind(L"@{", 0) == 0) {
                app.icon = ExtractPackagedIcon(factory.Get(), location, size);
            } else if (!location.empty()) {
                app.icon = ExtractClassicIcon(factory.Get(), location, app.iconIndex, size);
            }
            // If there is no icon location or it failed, try the program's own file (exe).
            const std::wstring exe = CleanPath(app.name);
            const bool isExe = exe.size() > 4 && _wcsicmp(exe.c_str() + exe.size() - 4, L".exe") == 0;
            if (!app.icon && isExe) app.icon = ExtractClassicIcon(factory.Get(), exe, 0, size);
            // Alias (MSIX) exes: first the package exe the alias points to, then the shell item's icon.
            if (!app.icon && isExe) {
                const std::wstring target = AppExecLinkTarget(exe);
                if (!target.empty()) app.icon = ExtractClassicIcon(factory.Get(), target, 0, size);
            }
            if (!app.icon && isExe) app.icon = ExtractShellItemIcon(factory.Get(), exe, size);
        }
    } catch (...) {
        DebugLog(L"Open with: could not load icons");
    }
}

HRESULT InvokeOpenWithApp(const OpenWithApp& app, const std::wstring& path) {
    if (!app.handler || path.empty()) return E_INVALIDARG;
    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf()));
    ComPtr<IDataObject> data;
    if (SUCCEEDED(hr)) hr = item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(data.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        AllowSetForegroundWindow(ASFW_ANY);
        hr = app.handler->Invoke(data.Get());
    }
    wchar_t message[512] = {};
    _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Open with: %s -> 0x%08lX", app.uiName.c_str(),
                 static_cast<unsigned long>(hr));
    DebugLog(message);
    return hr;
}

HRESULT ShellExecuteViaSite(IUnknown* site, const std::wstring& file, const std::wstring& args, const wchar_t* verb) {
    if (!site || file.empty()) return E_INVALIDARG;
    HRESULT hr = S_OK;
    const wchar_t* step = L"";
    try {
        ComPtr<IServiceProvider> provider;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> view;
        ComPtr<IDispatch> background;
        ComPtr<IShellFolderViewDual> folderView;
        ComPtr<IDispatch> application;
        ComPtr<IShellDispatch2> shell;
        step = L"IServiceProvider";
        hr = site->QueryInterface(IID_PPV_ARGS(provider.GetAddressOf()));
        if (SUCCEEDED(hr)) {
            step = L"SID_STopLevelBrowser";
            hr = provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(browser.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            step = L"QueryActiveShellView";
            hr = browser->QueryActiveShellView(view.GetAddressOf());
        }
        if (SUCCEEDED(hr)) {
            step = L"SVGIO_BACKGROUND";
            hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(background.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            step = L"IShellFolderViewDual";
            hr = background->QueryInterface(IID_PPV_ARGS(folderView.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            step = L"Application";
            hr = folderView->get_Application(application.GetAddressOf());
        }
        if (SUCCEEDED(hr)) {
            step = L"IShellDispatch2";
            hr = application->QueryInterface(IID_PPV_ARGS(shell.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            step = L"ShellExecute";
            BSTR fileBstr = SysAllocString(file.c_str());
            VARIANT vArgs{}, vDir{}, vVerb{}, vShow{};
            vArgs.vt = VT_BSTR;
            vArgs.bstrVal = SysAllocString(args.c_str());
            vDir.vt = VT_BSTR;
            vDir.bstrVal = SysAllocString(L"");
            vVerb.vt = VT_BSTR;
            vVerb.bstrVal = SysAllocString(verb ? verb : L"");
            vShow.vt = VT_I4;
            vShow.lVal = SW_SHOWNORMAL;
            if (!fileBstr || !vArgs.bstrVal || !vDir.bstrVal || !vVerb.bstrVal) {
                hr = E_OUTOFMEMORY;
            } else {
                AllowSetForegroundWindow(ASFW_ANY);
                hr = shell->ShellExecute(fileBstr, vArgs, vDir, vVerb, vShow);
            }
            SysFreeString(fileBstr);
            VariantClear(&vArgs);
            VariantClear(&vDir);
            VariantClear(&vVerb);
        }
    } catch (...) {
        hr = E_UNEXPECTED;
    }
    wchar_t message[600] = {};
    _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Open via Explorer (%s): %s %s -> step %s 0x%08lX",
                 verb ? verb : L"default", file.c_str(), args.c_str(), step, static_cast<unsigned long>(hr));
    DebugLog(message);
    return hr;
}

bool OpenWithAppCommand(const OpenWithApp& app, const std::wstring& path, std::wstring& file, std::wstring& args) {
    const std::wstring& exe = app.name;
    if (exe.size() < 5 || PathIsRelativeW(exe.c_str())) return false;
    if (_wcsicmp(exe.c_str() + exe.size() - 4, L".exe") != 0) return false;
    const DWORD attributes = GetFileAttributesW(exe.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    file = exe;
    args = L"\"" + path + L"\"";
    return true;
}

bool IsLowIntegrityProcess() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    BYTE buffer[64] = {};
    DWORD size = 0;
    bool low = false;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer);
        const DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
        const DWORD rid = *GetSidSubAuthority(label->Label.Sid, count - 1);
        low = rid < SECURITY_MANDATORY_MEDIUM_RID;
    }
    CloseHandle(token);
    return low;
}

HRESULT ShowOpenWithDialog(HWND owner, const std::wstring& path) {
    if (path.empty()) return E_INVALIDARG;
    OPENASINFO info{};
    info.pcszFile = path.c_str();
    info.pcszClass = nullptr;
    // OAIF_HIDE_REGISTRATION: no "Always use this app" checkbox; the default program never changes.
    info.oaifInFlags = OAIF_EXEC | OAIF_HIDE_REGISTRATION;
    AllowSetForegroundWindow(ASFW_ANY);
    const HRESULT hr = SHOpenWithDialog(owner, &info);
    wchar_t message[128] = {};
    _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Open with dialog -> 0x%08lX",
                 static_cast<unsigned long>(hr));
    DebugLog(message);
    return hr;
}

bool IsUsableOpenWithPath(const std::wstring& path) {
    if (path.size() < 4) return false;
    const bool drive = std::iswalpha(path[0]) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    const bool unc = path[0] == L'\\' && path[1] == L'\\';
    if (!drive && !unc) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    return IsSupportedModelExtension(path.c_str());
}
