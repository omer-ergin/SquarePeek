#pragma once

#include "com_ids.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <wrl/client.h>

class PreviewWindow;

// Preview handler. Runs IN-PROCESS in Explorer's shared prevhost.exe, at medium integrity level.
// Every COM entry point catches exceptions; DoPreview returns immediately (the model loads on a separate thread).
class PreviewHandler final : public IPreviewHandler,
                             public IPreviewHandlerVisuals,
                             public IInitializeWithFile,
                             public IInitializeWithStream,
                             public IInitializeWithItem,
                             public IObjectWithSite,
                             public IOleWindow {
public:
    PreviewHandler();
    ~PreviewHandler();

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    IFACEMETHODIMP SetWindow(HWND hwndParent, const RECT* prc) override;
    IFACEMETHODIMP SetRect(const RECT* prc) override;
    IFACEMETHODIMP DoPreview() override;
    IFACEMETHODIMP Unload() override;
    IFACEMETHODIMP SetFocus() override;
    IFACEMETHODIMP QueryFocus(HWND* phwnd) override;
    IFACEMETHODIMP TranslateAccelerator(MSG* pmsg) override;

    IFACEMETHODIMP SetBackgroundColor(COLORREF color) override;
    IFACEMETHODIMP SetFont(const LOGFONTW* plf) override;
    IFACEMETHODIMP SetTextColor(COLORREF color) override;

    IFACEMETHODIMP Initialize(LPCWSTR filePath, DWORD grfMode) override;

    IFACEMETHODIMP Initialize(IStream* stream, DWORD grfMode) override;

    // Shell item (Explorer uses this; QueryInterface does not expose IInitializeWithStream): if there is a file
    // system path the file is read from it (no stream is bound); otherwise the item's stream (BHID_Stream) is read.
    IFACEMETHODIMP Initialize(IShellItem* item, DWORD grfMode) override;

    IFACEMETHODIMP SetSite(IUnknown* site) override;
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override;

    IFACEMETHODIMP GetWindow(HWND* phwnd) override;
    IFACEMETHODIMP ContextSensitiveHelp(BOOL enterMode) override;

private:
    // Creates the window object if missing and hands it the frame (IPreviewHandlerFrame).
    PreviewWindow& EnsureWindow();

    std::atomic<LONG> m_refs{1};
    // shared_ptr: if Unload releases the window while a message is being handled (WndProc), the object lives
    // until the frames on the stack finish.
    std::shared_ptr<PreviewWindow> m_window;
    // Last SetWindow: Unload destroys the window; it is rebuilt if the shell calls DoPreview without SetWindow.
    HWND m_parent = nullptr;
    RECT m_rect{};
    // Site and frame are released in SetSite(nullptr) / the destructor, NOT in Unload (the shell may reuse the
    // same handler after Unload with a new Initialize + DoPreview).
    Microsoft::WRL::ComPtr<IUnknown> m_site;
    // The shell's preview frame: keys we don't use (Tab, arrows, ...) are forwarded to it.
    Microsoft::WRL::ComPtr<IPreviewHandlerFrame> m_frame;
};
