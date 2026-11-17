#include "com_ids.h"
#include "preview_handler.h"
#include "registration.h"
#include "thumbnail_provider.h"

#include <new>
#include <string>

HMODULE g_moduleHandle = nullptr;

namespace {
template <typename T>
class ClassFactory final : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }

    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;

        auto* object = new (std::nothrow) T();
        if (!object) return E_OUTOFMEMORY;

        const HRESULT hr = object->QueryInterface(riid, ppv);
        object->Release();
        return hr;
    }

    // No lock count: the DLL is never unloaded (DllCanUnloadNow).
    IFACEMETHODIMP LockServer(BOOL) override { return S_OK; }
};

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_moduleHandle = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (!IsEqualGUID(clsid, CLSID_StlThumbnailProvider) && !IsEqualGUID(clsid, CLSID_StlPreviewHandler)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    IClassFactory* factory = nullptr;
    if (IsEqualGUID(clsid, CLSID_StlThumbnailProvider)) {
        static ClassFactory<ThumbnailProvider> thumbnailFactory;
        factory = &thumbnailFactory;
    } else {
        static ClassFactory<PreviewHandler> previewFactory;
        factory = &previewFactory;
    }

    return factory->QueryInterface(riid, ppv);
}

// Load / launch threads run detached and depend on DLL code, so the DLL is never unloaded.
STDAPI DllCanUnloadNow() {
    return S_FALSE;
}

// regsvr32 is not supported: an HKCU registration shadows the machine-wide one and drops the preview to low
// integrity. Install with: stlprev-setup register-machine (administrator).
STDAPI DllRegisterServer() { return E_NOTIMPL; }

STDAPI DllUnregisterServer() {
    std::wstring log;
    const bool ok = UnregisterShellExtension(log);
    OutputDebugStringW(log.c_str());
    return ok ? S_OK : E_FAIL;
}
