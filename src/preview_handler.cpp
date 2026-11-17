#include "preview_handler.h"

#include "com_util.h"
#include "i18n.h"
#include "model.h"
#include "open_with.h"
#include "overlay.h"
#include "renderer.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include <dwmapi.h>
#include <shellapi.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kWindowClass[] = L"STLPreviewExplorerPane";
constexpr wchar_t kWindowTitle[] = L"SquarePeek";
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kFrameIntervalMs = 33;
// While the pane is not visible (hidden, minimized, on another desktop) the 33 ms timer stops; it only polls
// at this interval so it can resume right away once visible.
constexpr UINT kHiddenPollMs = 500;
// Notifications from worker threads (load finished / program failed to launch).
constexpr UINT kMsgLoadDone = WM_APP + 1;
constexpr UINT kMsgLaunchFailed = WM_APP + 2;
constexpr float kOrbitSpeed = 14.0f;
constexpr float kDragDegreesPerPixel = 0.45f;
constexpr ULONGLONG kAutoRotateDelayMs = 2500;
constexpr uint32_t kPreviewTriangleBudget = 4'000'000;
// Fitting is smoothed (0.15), so this many extra frames are drawn after each change.
constexpr int kSettleFrames = 20;
constexpr ULONGLONG kToastVisibleMs = 1400;
constexpr ULONGLONG kToastFadeMs = 400;
// Render / swap chain size limits: texture edge at most D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION (16384),
// area at most ~8 MP (DXGI_SCALING_STRETCH upscales in larger panes); MSAA off above ~4 MP.
constexpr UINT kMaxRenderDimension = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
constexpr double kMaxRenderPixels = 8.0e6;
constexpr double kMsaaMaxPixels = 4.0e6;
// Retry interval if the device / swap chain can't be set up (doubles on each failure, at most 5 s).
constexpr ULONGLONG kRetryInitialMs = 250;
constexpr ULONGLONG kRetryMaxMs = 5000;

DirectX::XMFLOAT3 FromColorRef(COLORREF color) {
    return {GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f};
}

std::wstring FileNameFromPath(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring OnOff(bool value) {
    return value ? L10n(L"açık", L"on") : L10n(L"kapalı", L"off");
}

bool CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* destination = GlobalLock(memory);
            if (destination) {
                std::memcpy(destination, text.c_str(), bytes);
                GlobalUnlock(memory);
                ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            }
            // If SetClipboardData succeeds, the memory now belongs to the system.
            if (!ok) GlobalFree(memory);
        }
    }
    CloseClipboard();
    return ok;
}

// Is this process at low integrity level? (Computed once.) At low level NO program is launched:
// the "Open with" bar is hidden and Enter is ignored (a program launched from there would also run at low level).
bool ProcessIsLowIntegrity() {
    static const bool low = IsLowIntegrityProcess();
    return low;
}

// At COM entry points, matches the thread's DPI context to the window's (prevhost is system-DPI aware; the pane
// window may have been created with its own context). Restores the old context in the destructor.
class ThreadDpiScope {
public:
    explicit ThreadDpiScope(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return;
        const DPI_AWARENESS_CONTEXT context = GetWindowDpiAwarenessContext(hwnd);
        if (context) m_previous = SetThreadDpiAwarenessContext(context);
    }
    ~ThreadDpiScope() {
        if (m_previous) SetThreadDpiAwarenessContext(m_previous);
    }
    ThreadDpiScope(const ThreadDpiScope&) = delete;
    ThreadDpiScope& operator=(const ThreadDpiScope&) = delete;

private:
    DPI_AWARENESS_CONTEXT m_previous = nullptr;
};

enum class KeyAction {
    None,
    View,
    Grid,
    BBox,
    Plate,
    Overhang,
    Wireframe,
    Edges,
    AutoRotate,
    Info,
    Help,
    CloseHelp,
    CloseSettings,
    Copy,
    OpenWithBar,
    OpenDefault,
};

// A mouse that moves more than this (pixels) between press and release on an "Open with" button is not a click.
constexpr LONG kClickSlopPx = 4;

// Programs are launched on a separate STA thread (IAssocHandler::Invoke / ShellExecuteEx may pump messages and
// must not block prevhost's thread). One launch at a time; for one double-click interval after it ends, new
// launches are ignored too (so a triple click doesn't open twice). Process-wide.
std::atomic<bool> g_launchInProgress{false};
std::atomic<ULONGLONG> g_lastLaunchEnd{0};

bool LaunchBlocked() {
    if (g_launchInProgress.load()) return true;
    const ULONGLONG last = g_lastLaunchEnd.load();
    return last != 0 && GetTickCount64() - last < static_cast<ULONGLONG>(GetDoubleClickTime());
}

struct LaunchRequest {
    bool defaultProgram = false;   // true: the file's default verb (same as double-click)
    std::wstring path;
    std::wstring appName;          // IAssocHandler::GetName (re-enumerated and matched by name)
    std::wstring appUiName;
    HWND notify = nullptr;         // receives kMsgLaunchFailed on failure
};

void RunLaunch(const LaunchRequest& request) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HRESULT hr = E_FAIL;
    try {
        AllowSetForegroundWindow(ASFW_ANY);
        if (request.defaultProgram) {
            // NO owner window (modal loops deadlock if Explorer's cross-process root window is the owner).
            SHELLEXECUTEINFOW execute{};
            execute.cbSize = sizeof(execute);
            execute.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            execute.hwnd = nullptr;
            execute.lpFile = request.path.c_str();
            execute.nShow = SW_SHOWNORMAL;
            hr = ShellExecuteExW(&execute) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        } else {
            // IAssocHandler objects are bound to the thread that enumerated them: re-enumerate on this thread
            // and find the program by name (only handlers registered for this extension can be opened).
            const OpenWithList list = EnumerateOpenWithApps(PathFindExtensionW(request.path.c_str()), 64, true);
            hr = E_ACCESSDENIED;
            for (const OpenWithApp& app : list.apps) {
                const bool match = !request.appName.empty()
                                       ? _wcsicmp(app.name.c_str(), request.appName.c_str()) == 0
                                       : _wcsicmp(app.uiName.c_str(), request.appUiName.c_str()) == 0;
                if (match) {
                    hr = InvokeOpenWithApp(app, request.path);
                    break;
                }
            }
        }
    } catch (...) {
        hr = E_UNEXPECTED;
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (FAILED(hr)) {
        wchar_t message[160] = {};
        _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Could not launch program (%s): 0x%08lX",
                     request.defaultProgram ? L"default" : L"open with", static_cast<unsigned long>(hr));
        DebugLog(message);
        if (request.notify) PostMessageW(request.notify, kMsgLaunchFailed, 0, 0);
    }
    g_lastLaunchEnd.store(GetTickCount64());
    g_launchInProgress.store(false);
}

// Starts the launch on a short-lived thread. Returns false if one is already running.
bool StartLaunch(LaunchRequest request) {
    bool expected = false;
    if (!g_launchInProgress.compare_exchange_strong(expected, true)) return false;
    try {
        std::thread([request = std::move(request)]() { RunLaunch(request); }).detach();
        return true;
    } catch (...) {
        g_launchInProgress.store(false);
        return false;
    }
}

// Windows' "Open with" dialog is opened in a SEPARATE process (rundll32 shell32.dll,OpenAs_RunDLL): it is not
// tied to a thread kept alive in prevhost or to Explorer's window. The path is passed unquoted
// (OpenAs_RunDLL takes the rest of the command line as the file name; paths with spaces work too).
bool OpenOpenWithDialogProcess(const std::wstring& path) {
    wchar_t system[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
    if (length == 0 || length >= ARRAYSIZE(system)) return false;
    const std::wstring directory(system, length);
    const std::wstring exe = directory + L"\\rundll32.exe";
    std::wstring commandLine = L"\"" + exe + L"\" \"" + directory + L"\\shell32.dll\",OpenAs_RunDLL " + path;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process)) {
        wchar_t message[96] = {};
        _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Could not open the Open with dialog (code %lu)",
                     GetLastError());
        DebugLog(message);
        return false;
    }
    AllowSetForegroundWindow(process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

// --- Async loading ---
// A load job: reading + LoadModel run on a separate thread; the result waits here and kMsgLoadDone is posted
// to the window. Window and thread share the job via shared_ptr (if the window goes away, the job finishes on
// its own and is released). Only one preview load runs at a time (peak memory): a queued job that was
// cancelled by the time it gets the lock reads nothing.
struct LoadJob {
    uint32_t generation = 0;
    std::atomic<bool> cancel{false};
    HWND notify = nullptr;
    std::wstring path;          // if non-empty, the file is read from here
    IStream* marshaledStream = nullptr;   // if no path: CoMarshalInterThreadInterfaceInStream packet
    std::wstring formatHint;    // file name (format hint); may be empty

    std::mutex mutex;
    bool done = false;
    bool ok = false;
    Mesh mesh;
    std::wstring error;
    ULONGLONG elapsedMs = 0;

    ~LoadJob() {
        if (marshaledStream) {
            CoReleaseMarshalData(marshaledStream);
            marshaledStream->Release();
        }
    }
};

std::mutex g_previewLoadMutex;

void RunLoadJob(const std::shared_ptr<LoadJob>& job) {
    const ULONGLONG start = GetTickCount64();
    bool ok = false;
    Mesh mesh;
    std::wstring error;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        // The stream packet is always unmarshaled (even on cancel; otherwise it leaks).
        ComPtr<IStream> stream;
        if (job->marshaledStream) {
            IStream* packet = job->marshaledStream;
            job->marshaledStream = nullptr;
            if (FAILED(CoGetInterfaceAndReleaseStream(packet, IID_PPV_ARGS(stream.GetAddressOf())))) stream.Reset();
        }
        std::lock_guard<std::mutex> serialize(g_previewLoadMutex);
        if (job->cancel.load()) {
            error = L"İptal edildi";
        } else {
            std::vector<uint8_t> bytes;
            bool read = false;
            if (!job->path.empty()) {
                read = ReadFileAll(job->path.c_str(), bytes, error);
            } else if (stream) {
                LARGE_INTEGER zero{};
                stream->Seek(zero, STREAM_SEEK_SET, nullptr);
                read = ReadStreamAll(stream.Get(), bytes, error);
            } else {
                error = L"Veri kaynağı açılamadı";
            }
            stream.Reset();
            if (read && job->cancel.load()) {
                read = false;
                error = L"İptal edildi";
            }
            if (read) {
                const wchar_t* hint = job->formatHint.empty() ? nullptr : job->formatHint.c_str();
                ok = LoadModel(bytes.data(), bytes.size(), hint, kPreviewTriangleBudget, mesh, error,
                               kModelLoadDefault, kPreviewLoadBudgetMs);
            }
        }
    } catch (...) {
        ok = false;
        mesh = Mesh{};
        error = L"Bellek yetersiz";
    }
    if (SUCCEEDED(init)) CoUninitialize();
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->ok = ok;
        job->mesh = std::move(mesh);
        job->error = std::move(error);
        job->elapsedMs = GetTickCount64() - start;
        job->done = true;
    }
    if (!job->cancel.load()) PostMessageW(job->notify, kMsgLoadDone, 0, 0);
}
}  // namespace

class PreviewWindow : public std::enable_shared_from_this<PreviewWindow> {
public:
    PreviewWindow();
    ~PreviewWindow() { Destroy(); }

    bool Create(HWND parent, const RECT& rect);
    void Destroy();
    void SetRect(const RECT& rect);
    void SetSource(std::wstring path, ComPtr<IStream> stream, const std::wstring& openPathHint = std::wstring());
    void SetBackgroundColor(COLORREF color);
    void SetFrame(ComPtr<IPreviewHandlerFrame> frame) { m_frame = std::move(frame); }
    bool StartPreview();
    void StopPreview();
    void Focus() const {
        if (m_hwnd) ::SetFocus(m_hwnd);
    }
    HWND Hwnd() const { return m_hwnd; }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    enum class LoadState { Idle, Loading, Ready, Failed };

    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);
    void OnNcDestroy();

    // --- loading ---
    void BeginLoad();
    void CancelLoad();
    void OnLoadDone();
    // Hands the loaded model to the renderer (UI thread). Deferred if there is no device.
    bool UploadScene();

    // --- graphics ---
    // Device + renderer + overlay + swap chain. On failure releases resources and schedules a retry with
    // backoff (force: ignore the backoff).
    bool EnsureGraphics(bool force);
    void ScheduleRetry();
    void ResetRetry();
    void ReleaseDevice();
    bool InitializeSwapChain();
    bool CreateBackBuffer();
    void ResizeSwapChain();
    // Swap chain / render size from the client size (edge and area limits).
    void ComputeRenderSize(UINT clientWidth, UINT clientHeight, UINT& width, UINT& height) const;
    void RenderFrame();
    void OnTimer();
    void SetFrameTimer(UINT intervalMs);
    void ResetCamera();
    void MarkInteraction();
    void ShowToast(std::wstring text);
    void EndMouseCapture();
    void ReloadSettings();

    // --- "Open with" bar ---
    // Lists programs (and their icons) once per file; does nothing if there is no file path.
    void EnsureOpenWith();
    // Re-extracts icons if the icon size changed with DPI.
    void RefreshOpenWithIcons(float dpiScale);
    // Fills m_info.openWith (with hover / pressed state).
    void FillOpenWithButtons();
    void ClearOpenWithState();
    // Topmost clickable item under the point (client pixels) in the last drawn frame.
    OverlayTarget HitTest(POINT point) const;
    void UpdateHover(POINT point);
    void ActivateButton(int button);
    void ActivateTarget(OverlayTarget target);
    void SetSettingsOpen(bool open);
    void OpenWithDefaultProgram();
    // Is the pane actually visible (not hidden, minimized or on another virtual desktop)?
    bool IsPaneVisible() const;

    // Handles the key and returns true if it is one of ours (WM_KEYDOWN / WM_SYSKEYDOWN).
    bool HandleKey(UINT message, WPARAM wParam, LPARAM lParam);
    void ApplyKey(KeyAction action, ViewPreset preset);

    HWND m_hwnd = nullptr;
    HWND m_parent = nullptr;
    bool m_windowCounted = false;

    // Shared device (renderer.h); only the swap chain, renderer resources and D2D context belong to the window.
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    bool m_deviceAcquired = false;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_backBuffer;
    ComPtr<ID3D11Texture2D> m_backBufferTexture;
    UINT m_bufferWidth = 0;
    UINT m_bufferHeight = 0;
    // Render pixels / client pixels (may be < 1 due to the limits; mouse coordinates are scaled by it).
    float m_renderScale = 1.0f;
    ULONGLONG m_retryAt = 0;
    ULONGLONG m_retryDelay = kRetryInitialMs;

    Renderer m_renderer;
    bool m_rendererReady = false;
    Overlay m_overlay;
    bool m_overlayReady = false;
    OverlayInfo m_info;
    PreviewSettings m_settings;
    Mesh m_mesh;
    CameraState m_camera;

    std::wstring m_path;
    std::wstring m_fileName;
    ComPtr<IStream> m_stream;
    ComPtr<IPreviewHandlerFrame> m_frame;

    LoadState m_loadState = LoadState::Idle;
    std::shared_ptr<LoadJob> m_job;
    std::wstring m_loadError;
    bool m_sceneReady = false;
    // Whether m_mesh's vertices are in memory (for a stream source a CPU copy is kept for device loss; for a file
    // path it is released after upload and the file is re-read on device loss).
    bool m_meshReady = false;
    // Incremented whenever the source or window changes; stale load results are discarded.
    uint32_t m_generation = 0;
    // Present returned DXGI_STATUS_OCCLUDED: nothing is drawn until visible, only polled.
    bool m_occluded = false;
    bool m_cameraInitialized = false;
    bool m_previewing = false;
    UINT m_timerInterval = 0;   // 0: no timer
    bool m_dirty = false;
    bool m_dragging = false;
    bool m_panning = false;
    bool m_rotateHold = false;
    bool m_showHelp = false;
    int m_settleFrames = 0;
    std::wstring m_toast;
    ULONGLONG m_toastStart = 0;
    POINT m_lastMouse{};
    ULONGLONG m_lastInput = 0;
    ULONGLONG m_lastFrame = 0;

    // "Open with": only if the full file path is known and the process is not at low integrity.
    std::wstring m_openPath;
    OpenWithList m_openWith;
    bool m_openWithEnumerated = false;
    int m_openWithIconPx = 0;
    std::wstring m_launchName;            // name of the last launched program (for the failure notice)
    std::vector<OverlayHitRect> m_hits;   // clickable items drawn in the last frame (last is on top; render px)
    OverlayTarget m_hover;
    OverlayTarget m_press;                // left button was pressed over this item
    bool m_pressMoved = false;            // moved more than kClickSlopPx while pressed (not a click)
    // Settings panel (gear). Auto-rotate stops while open; a click outside only closes it.
    bool m_showSettings = false;
    // The double-click message of the click that closed the panel by clicking outside is ignored (no view reset).
    bool m_ignoreDoubleClick = false;
    POINT m_pressPoint{};
    bool m_trackingLeave = false;
    // A key forwarded to the frame is not forwarded again if it comes back from the frame (recursion guard).
    bool m_forwardingKey = false;
};

PreviewWindow::PreviewWindow() {
    m_camera.background = FromColorRef(SystemThemeBackgroundColor());
}

bool PreviewWindow::Create(HWND parent, const RECT& rect) {
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, [] {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &PreviewWindow::StaticWndProc;
        windowClass.hInstance = g_moduleHandle;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kWindowClass;
        registered = RegisterClassExW(&windowClass) != 0;
    });
    if (!registered) {
        DebugLog((L"RegisterClassExW failed, code " + std::to_wstring(GetLastError())).c_str());
        return false;
    }

    if (m_hwnd && IsWindow(m_hwnd)) {
        if (parent != m_parent && parent) {
            // The shell gave a new parent window (pane recreated): the window is moved; device, model and
            // preview state are kept (the swap chain is bound to the window, not the parent).
            const LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
            if (!(style & WS_CHILD))
                SetWindowLongPtrW(m_hwnd, GWL_STYLE, (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD);
            if (!SetParent(m_hwnd, parent)) {
                DebugLog((L"SetParent failed, code " + std::to_wstring(GetLastError())).c_str());
            } else {
                m_parent = parent;
            }
        }
        SetWindowPos(m_hwnd, HWND_TOP, rect.left, rect.top, std::max<LONG>(rect.right - rect.left, 1),
                     std::max<LONG>(rect.bottom - rect.top, 1),
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | (m_previewing ? SWP_SHOWWINDOW : 0));
        m_dirty = true;
        return true;
    }
    Destroy();

    m_parent = parent;
    const DWORD style = parent ? (WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN)
                               : (WS_POPUP | WS_CLIPCHILDREN);

    m_hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, style, rect.left, rect.top,
                             std::max<LONG>(rect.right - rect.left, 1),
                             std::max<LONG>(rect.bottom - rect.top, 1), parent, nullptr, g_moduleHandle,
                             this);
    if (!m_hwnd && parent) {
        const DWORD firstError = GetLastError();
        DebugLog((L"CreateWindowExW (with parent) failed, code " + std::to_wstring(firstError)).c_str());

        m_hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_POPUP | WS_CLIPCHILDREN, rect.left,
                                 rect.top, std::max<LONG>(rect.right - rect.left, 1),
                                 std::max<LONG>(rect.bottom - rect.top, 1), nullptr, nullptr, g_moduleHandle,
                                 this);
        if (m_hwnd) SetParent(m_hwnd, parent);
    }
    if (!m_hwnd) {
        DebugLog((L"CreateWindowExW failed, code " + std::to_wstring(GetLastError())).c_str());
        return false;
    }
    m_windowCounted = true;
    m_settings = LoadPreviewSettings();
    return true;
}

void PreviewWindow::Destroy() {
    if (m_hwnd) {
        const HWND hwnd = m_hwnd;
        // Cleanup happens in WM_NCDESTROY (OnNcDestroy), even if the window is destroyed another way (parent).
        if (GetCapture() == hwnd) ReleaseCapture();
        if (!DestroyWindow(hwnd)) OnNcDestroy();
    }
    ++m_generation;
    CancelLoad();
    ReleaseDevice();
    m_sceneReady = false;
    m_previewing = false;
    m_occluded = false;
    m_dragging = false;
    m_panning = false;
    m_parent = nullptr;
    // The program list, IAssocHandlers and icons are released (Unload).
    ClearOpenWithState();
}

void PreviewWindow::OnNcDestroy() {
    // L10: the window was destroyed (Destroy or the parent's destruction). User data is cleared; the swap chain
    // and the shared device reference are released; a pending load is cancelled.
    if (m_hwnd) {
        KillTimer(m_hwnd, kAnimationTimer);
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
    }
    m_timerInterval = 0;
    m_previewing = false;
    m_hwnd = nullptr;
    m_windowCounted = false;
    ++m_generation;
    CancelLoad();
    ReleaseDevice();
}

void PreviewWindow::ClearOpenWithState() {
    m_openWith = OpenWithList{};
    m_openWithEnumerated = false;
    m_openWithIconPx = 0;
    m_hits.clear();
    m_info.openWith.clear();
    m_hover = OverlayTarget{};
    m_press = OverlayTarget{};
    m_pressMoved = false;
    m_trackingLeave = false;   // TME_LEAVE is requested again on the new window
    // File changed (SetSource) or window gone (Destroy): the settings panel closes.
    m_showSettings = false;
    m_ignoreDoubleClick = false;
}

void PreviewWindow::SetRect(const RECT& rect) {
    if (!m_hwnd) return;
    SetWindowPos(m_hwnd, nullptr, rect.left, rect.top, std::max<LONG>(rect.right - rect.left, 1),
                 std::max<LONG>(rect.bottom - rect.top, 1), SWP_NOZORDER | SWP_NOACTIVATE);
}

void PreviewWindow::SetSource(std::wstring path, ComPtr<IStream> stream, const std::wstring& openPathHint) {
    ++m_generation;
    CancelLoad();
    m_path = std::move(path);
    m_stream = std::move(stream);
    m_fileName.clear();
    ClearOpenWithState();
    m_openPath.clear();
    const bool canLaunch = !ProcessIsLowIntegrity();
    if (!m_path.empty()) {
        m_fileName = FileNameFromPath(m_path);
        if (canLaunch && IsUsableOpenWithPath(m_path)) m_openPath = m_path;
    } else if (m_stream && !openPathHint.empty()) {
        m_fileName = FileNameFromPath(openPathHint);
        if (canLaunch && IsUsableOpenWithPath(openPathHint)) m_openPath = openPathHint;
    } else if (m_stream) {
        STATSTG stat{};
        if (SUCCEEDED(m_stream->Stat(&stat, STATFLAG_DEFAULT)) && stat.pwcsName) {
            const std::wstring name = stat.pwcsName;
            CoTaskMemFree(stat.pwcsName);
            m_fileName = FileNameFromPath(name);
            // Only a full, existing path; a bare file name is never turned into a guessed path (bar stays hidden).
            if (canLaunch && IsUsableOpenWithPath(name)) m_openPath = name;
        }
    }
    m_mesh = Mesh{};
    m_meshReady = false;
    m_sceneReady = false;
    m_cameraInitialized = false;
    m_loadState = LoadState::Idle;
    m_loadError.clear();
    m_info = OverlayInfo{};
    // Release the previous file's GPU buffers right away (no two models in memory while the new one loads).
    if (m_rendererReady) {
        std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
        m_renderer.ReleaseResources();
    }
    m_dirty = true;
}

void PreviewWindow::SetBackgroundColor(COLORREF color) {
    m_camera.background = FromColorRef(color);
    m_dirty = true;
}

// ---------------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------------

void PreviewWindow::CancelLoad() {
    if (m_job) {
        m_job->cancel.store(true);
        m_job.reset();
    }
    if (m_loadState == LoadState::Loading) m_loadState = LoadState::Idle;
}

void PreviewWindow::BeginLoad() {
    if (m_meshReady) {
        // Stream source: the CPU copy is still there (device loss); not re-read.
        m_loadState = LoadState::Ready;
        UploadScene();
        return;
    }
    CancelLoad();
    m_loadError.clear();
    if (!m_hwnd) return;
    if (m_path.empty() && !m_stream) {
        m_loadState = LoadState::Failed;
        m_loadError = L"Veri kaynağı belirtilmedi";
        m_dirty = true;
        return;
    }
    auto job = std::make_shared<LoadJob>();
    job->generation = m_generation;
    job->notify = m_hwnd;
    job->path = m_path;
    job->formatHint = m_fileName;
    if (job->path.empty()) {
        // The stream was received on this (STA) thread: marshal it to the load thread.
        if (FAILED(CoMarshalInterThreadInterfaceInStream(IID_IStream, m_stream.Get(), &job->marshaledStream))) {
            job->marshaledStream = nullptr;
            m_loadState = LoadState::Failed;
            m_loadError = L"Veri akışı iş parçacığına aktarılamadı";
            m_dirty = true;
            return;
        }
    }
    try {
        std::thread([job]() { RunLoadJob(job); }).detach();
    } catch (...) {
        m_loadState = LoadState::Failed;
        m_loadError = L"Yükleme iş parçacığı başlatılamadı";
        m_dirty = true;
        return;
    }
    m_job = std::move(job);
    m_loadState = LoadState::Loading;
    m_dirty = true;
}

void PreviewWindow::OnLoadDone() {
    const std::shared_ptr<LoadJob> job = m_job;
    if (!job) return;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (!job->done) return;   // notification from an old (cancelled) job
    }
    m_job.reset();
    if (job->generation != m_generation) return;

    wchar_t message[512] = {};
    if (!job->ok) {
        m_loadState = LoadState::Failed;
        m_loadError = job->error;
        _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Preview failed to load (%s): %s", m_fileName.c_str(),
                     job->error.c_str());
        DebugLog(message);
    } else {
        m_mesh = std::move(job->mesh);
        m_meshReady = true;
        FillOverlayInfoFromMesh(m_info, m_mesh, m_fileName);
        m_loadState = LoadState::Ready;
        // Stream source: release the stream right away (holding it blocks overwriting the file; CAD/slicer can't
        // re-export). It can't be re-read after device loss, so a decimated CPU copy is kept.
        if (m_path.empty()) m_stream.Reset();
        if (job->elapsedMs > 500) {
            _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Preview loaded: %llu ms, %llu triangles",
                         static_cast<unsigned long long>(job->elapsedMs),
                         static_cast<unsigned long long>(m_mesh.sourceTriangleCount));
            DebugLog(message);
        }
        UploadScene();
    }
    m_settleFrames = kSettleFrames;
    m_dirty = true;
    if (m_previewing) RenderFrame();
}

bool PreviewWindow::UploadScene() {
    if (m_sceneReady) return true;
    if (m_loadState != LoadState::Ready || !m_meshReady) return false;
    if (!m_rendererReady) return false;   // EnsureGraphics calls again once the device is up
    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
        ok = m_renderer.SetMesh(m_mesh);
    }
    if (!ok) {
        DebugLog(L"Could not upload model to GPU");
        // Device lost? If not (memory), the model can't be drawn: show the error frame.
        if (m_device && m_device->GetDeviceRemovedReason() != S_OK) {
            ReleaseDevice();
            ScheduleRetry();
        } else {
            m_loadState = LoadState::Failed;
            m_loadError = L"Model GPU belleğine sığmadı";
            m_dirty = true;
        }
        return false;
    }
    if (!m_path.empty()) {
        // File path: no handle is held; re-read from disk on device loss. Vertices and edges are on the GPU,
        // stats stay in Mesh; CPU copies are not needed.
        std::vector<float>().swap(m_mesh.vertices);
        std::vector<float>().swap(m_mesh.featureEdges);
        m_meshReady = false;
    }
    m_sceneReady = true;
    if (!m_cameraInitialized) {
        ResetCamera();
        m_cameraInitialized = true;
    }
    m_settleFrames = kSettleFrames;
    m_dirty = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Graphics
// ---------------------------------------------------------------------------------------------

void PreviewWindow::ScheduleRetry() {
    m_retryAt = GetTickCount64() + m_retryDelay;
    m_retryDelay = std::min(m_retryDelay * 2, kRetryMaxMs);
}

void PreviewWindow::ResetRetry() {
    m_retryAt = 0;
    m_retryDelay = kRetryInitialMs;
}

bool PreviewWindow::EnsureGraphics(bool force) {
    if (!m_hwnd) return false;
    if (m_rendererReady && m_swapChain && m_backBuffer) {
        if (m_loadState == LoadState::Ready && !m_sceneReady) {
            if (m_meshReady) {
                UploadScene();
            } else {
                BeginLoad();   // file path: the model is re-read after device loss
            }
        }
        return true;
    }
    if (!force && GetTickCount64() < m_retryAt) return false;

    bool ok = true;
    const wchar_t* step = L"";
    if (!m_device) {
        if (!AcquireSharedGpuDevice(m_device)) {
            ok = false;
            step = L"Direct3D device";
        } else {
            m_deviceAcquired = true;
            m_device->GetImmediateContext(m_context.GetAddressOf());
        }
    }
    if (ok && !m_rendererReady) {
        std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
        m_rendererReady = m_renderer.Initialize(m_device.Get());
        if (!m_rendererReady) {
            ok = false;
            step = L"renderer";
        } else {
            // The overlay is optional: if it fails, the preview runs without text.
            m_overlayReady = m_overlay.Initialize(m_device.Get());
        }
    }
    if (ok && !m_swapChain && !InitializeSwapChain()) {
        ok = false;
        step = L"swap chain";
    }
    if (!ok) {
        wchar_t message[160] = {};
        _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Graphics setup failed (%s); retrying in %llu ms",
                     step, static_cast<unsigned long long>(m_retryDelay));
        DebugLog(message);
        ReleaseDevice();
        ScheduleRetry();
        return false;
    }
    ResetRetry();
    m_dirty = true;
    m_settleFrames = kSettleFrames;
    if (m_loadState == LoadState::Ready && !m_sceneReady) {
        if (m_meshReady) {
            UploadScene();
        } else {
            BeginLoad();
        }
    }
    return true;
}

void PreviewWindow::ReleaseDevice() {
    {
        std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
        // The icons' D2D bitmaps go with Overlay (CPU copies stay in m_openWith); there are no buttons to click
        // until the bar is drawn again.
        m_overlay.Reset();
        m_overlayReady = false;
        m_backBufferTexture.Reset();
        if (m_context) m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_backBuffer.Reset();
        m_swapChain.Reset();
        m_renderer.Reset();
        m_rendererReady = false;
        if (m_context) m_context->Flush();
        m_context.Reset();
        m_device.Reset();
    }
    if (m_deviceAcquired) {
        m_deviceAcquired = false;
        ReleaseSharedGpuDevice();
    }
    m_bufferWidth = 0;
    m_bufferHeight = 0;
    m_hits.clear();
    m_hover = OverlayTarget{};
    // The vertex buffer lived in the renderer; if the device is rebuilt the model must be uploaded again.
    m_sceneReady = false;
}

void PreviewWindow::ComputeRenderSize(UINT clientWidth, UINT clientHeight, UINT& width, UINT& height) const {
    const double w = std::max<double>(clientWidth, 1.0);
    const double h = std::max<double>(clientHeight, 1.0);
    double scale = std::min(1.0, static_cast<double>(kMaxRenderDimension) / std::max(w, h));
    if (w * h * scale * scale > kMaxRenderPixels) scale = std::sqrt(kMaxRenderPixels / (w * h));
    width = std::clamp<UINT>(static_cast<UINT>(std::floor(w * scale)), 1, kMaxRenderDimension);
    height = std::clamp<UINT>(static_cast<UINT>(std::floor(h * scale)), 1, kMaxRenderDimension);
}

bool PreviewWindow::InitializeSwapChain() {
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device.As(&dxgiDevice))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) return false;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) return false;

    RECT client{};
    GetClientRect(m_hwnd, &client);
    UINT width = 1;
    UINT height = 1;
    ComputeRenderSize(static_cast<UINT>(std::max<LONG>(client.right - client.left, 1)),
                      static_cast<UINT>(std::max<LONG>(client.bottom - client.top, 1)), width, height);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
    if (FAILED(factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &desc, nullptr, nullptr,
                                               m_swapChain.GetAddressOf()))) {
        m_swapChain.Reset();
        return false;
    }
    factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
    m_bufferWidth = width;
    m_bufferHeight = height;
    if (!CreateBackBuffer()) {
        m_swapChain.Reset();
        return false;
    }
    return true;
}

bool PreviewWindow::CreateBackBuffer() {
    if (!m_swapChain) return false;
    m_backBufferTexture.Reset();
    m_backBuffer.Reset();
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) return false;
    if (FAILED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, m_backBuffer.GetAddressOf())))
        return false;
    m_backBufferTexture = std::move(backBuffer);
    return true;
}

void PreviewWindow::ResizeSwapChain() {
    m_settleFrames = kSettleFrames;
    m_dirty = true;
    if (!m_hwnd) return;
    // Size changed: don't wait out the previous failure's backoff (e.g. a 20000 px pane got smaller).
    ResetRetry();
    if (!m_swapChain) {
        if (m_previewing) EnsureGraphics(true);
        return;
    }
    RECT client{};
    if (!GetClientRect(m_hwnd, &client)) return;
    if (client.right <= client.left || client.bottom <= client.top) return;
    UINT width = 1;
    UINT height = 1;
    ComputeRenderSize(static_cast<UINT>(client.right - client.left), static_cast<UINT>(client.bottom - client.top),
                      width, height);
    if (width == m_bufferWidth && height == m_bufferHeight && m_backBuffer) return;

    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
        // ResizeBuffers requires releasing ALL references to the back buffer (including the D2D bitmap).
        m_overlay.ReleaseTarget();
        m_backBuffer.Reset();
        m_backBufferTexture.Reset();
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_context->Flush();
        ok = SUCCEEDED(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)) && CreateBackBuffer();
    }
    if (!ok) {
        DebugLog(L"Could not resize swap chain; device will be recreated");
        ReleaseDevice();
        ScheduleRetry();
        return;
    }
    m_bufferWidth = width;
    m_bufferHeight = height;
}

void PreviewWindow::RenderFrame() {
    if (!m_hwnd || !m_swapChain || !m_backBuffer || !m_rendererReady) return;

    RECT client{};
    if (!GetClientRect(m_hwnd, &client)) return;
    const UINT clientWidth = static_cast<UINT>(client.right - client.left);
    const UINT clientHeight = static_cast<UINT>(client.bottom - client.top);
    if (clientWidth == 0 || clientHeight == 0) return;
    {
        UINT wantWidth = 1;
        UINT wantHeight = 1;
        ComputeRenderSize(clientWidth, clientHeight, wantWidth, wantHeight);
        if (wantWidth != m_bufferWidth || wantHeight != m_bufferHeight) {
            ResizeSwapChain();
            if (!m_swapChain || !m_backBuffer) return;
        }
    }
    const UINT width = m_bufferWidth;
    const UINT height = m_bufferHeight;
    m_renderScale = static_cast<float>(width) / static_cast<float>(clientWidth);

    const UINT dpi = GetDpiForWindow(m_hwnd);
    // Overlay and line width are in render pixels: same physical size when the render is downscaled.
    const float dpiScale = (dpi ? static_cast<float>(dpi) / 96.0f : 1.0f) * m_renderScale;
    const RenderOptions options = MakeRenderOptions(m_settings, dpiScale);
    const UINT samples = static_cast<double>(width) * height > kMsaaMaxPixels ? 1u : 4u;
    const bool showModel = m_loadState == LoadState::Ready && m_sceneReady;

    HRESULT presentResult = S_OK;
    {
        std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
        if (!m_renderer.Render(width, height, m_camera, options, showModel ? samples : 1u, 0.15f)) return;
        // The overlay is drawn onto the resolved texture, then COPIED to the back buffer (same size and format).
        // Drawing to the back buffer with a shader (Renderer::Blit) leaked 1 Event handle per swap chain (driver;
        // measured: 0 with the copy). D2D never binds to the back buffer either.
        ID3D11Texture2D* const resolved = m_renderer.ResolvedTexture();
        if (!resolved) return;
        m_context->OMSetRenderTargets(0, nullptr, nullptr);

        if (m_overlayReady) {
            const ULONGLONG elapsed = GetTickCount64() - m_toastStart;
            std::wstring toast;
            float toastAlpha = 1.0f;
            if (!m_toast.empty() && elapsed < kToastVisibleMs + kToastFadeMs) {
                toast = m_toast;
                toastAlpha = elapsed < kToastVisibleMs ? 1.0f
                                                       : 1.0f - static_cast<float>(elapsed - kToastVisibleMs) /
                                                                    static_cast<float>(kToastFadeMs);
            } else {
                m_toast.clear();
            }
            if (showModel) {
                FillOverlayInfoFromFrame(m_info, m_renderer.LastFrame(), m_mesh);
                m_info.settings = m_settings;
                m_info.canOpenWith = !m_openPath.empty();
                m_info.showHelp = m_showHelp;
                m_info.showSettings = m_showSettings;
                m_info.hover = m_hover.kind != OverlayHitKind::OpenWith ? m_hover : OverlayTarget{};
                // The pressed item looks pressed only while hovered and not moved.
                m_info.pressed =
                    (m_press.kind != OverlayHitKind::OpenWith && !m_pressMoved) ? m_press : OverlayTarget{};
                m_info.darkTheme = IsDarkBackground(m_camera.background);
                m_info.dpiScale = dpiScale;
                m_info.toast = toast;
                m_info.toastAlpha = toastAlpha;
                m_info.centerMessage.clear();
                m_info.centerDetail.clear();
                FillOpenWithButtons();
                m_overlay.Draw(resolved, m_info, &m_hits);
            } else {
                // Loading / error frame (nothing to click while there is no model).
                OverlayInfo status;
                status.darkTheme = IsDarkBackground(m_camera.background);
                status.dpiScale = dpiScale;
                status.toast = toast;
                status.toastAlpha = toastAlpha;
                if (m_loadState == LoadState::Failed) {
                    status.centerMessage = L10n(L"Bu dosya önizlenemedi", L"This file can't be previewed");
                    // The loader's error text is Turkish; the English UI shows the file name instead.
                    status.centerDetail = UiTurkish() && !m_loadError.empty() ? m_loadError : m_fileName;
                } else {
                    status.centerMessage = L10n(L"Yükleniyor…", L"Loading…");
                    status.centerDetail = m_fileName;
                }
                m_overlay.Draw(resolved, status, &m_hits);
            }
        } else if (!m_toast.empty() && GetTickCount64() - m_toastStart >= kToastVisibleMs + kToastFadeMs) {
            m_toast.clear();
        }
        m_context->CopyResource(m_backBufferTexture.Get(), resolved);
        presentResult = m_swapChain->Present(0, 0);
    }

    if (presentResult == DXGI_STATUS_OCCLUDED) {
        m_occluded = true;
    } else if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET) {
        DebugLog(L"Direct3D device lost; will be recreated");
        ReleaseDevice();
        ResetRetry();
        m_dirty = true;
    }
}

bool PreviewWindow::IsPaneVisible() const {
    if (!m_hwnd || !IsWindowVisible(m_hwnd)) return false;
    // IsWindowVisible also returns TRUE for child windows of a minimized window.
    const HWND root = GetAncestor(m_hwnd, GA_ROOT);
    if (!root) return true;
    if (IsIconic(root)) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    return true;
}

void PreviewWindow::SetFrameTimer(UINT intervalMs) {
    if (!m_hwnd || m_timerInterval == intervalMs) return;
    if (SetTimer(m_hwnd, kAnimationTimer, intervalMs, nullptr)) m_timerInterval = intervalMs;
}

void PreviewWindow::OnTimer() {
    const ULONGLONG now = GetTickCount64();
    // A hidden pane is not drawn; the 33 ms timer drops to slow polling (m_dirty is kept).
    if (!IsPaneVisible()) {
        m_lastFrame = now;
        SetFrameTimer(kHiddenPollMs);
        return;
    }
    SetFrameTimer(kFrameIntervalMs);
    if (m_occluded) {
        // Occluded window / locked screen: poll without drawing; resume once visible.
        bool stillOccluded = false;
        if (m_swapChain) {
            std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
            stillOccluded = m_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED;
        }
        if (stillOccluded) {
            m_lastFrame = now;
            return;
        }
        m_occluded = false;
        m_dirty = true;
    }
    const float delta = std::min(static_cast<float>(now - m_lastFrame) / 1000.0f, 0.1f);
    m_lastFrame = now;

    const bool modelShown = m_loadState == LoadState::Ready && m_sceneReady;
    bool needsFrame = m_dirty || m_settleFrames > 0 || !m_toast.empty();
    m_dirty = false;
    if (m_settleFrames > 0) --m_settleFrames;

    if (modelShown && m_settings.autoRotate && !m_rotateHold && !m_dragging && !m_panning && !m_showHelp &&
        !m_showSettings && now - m_lastInput > kAutoRotateDelayMs) {
        m_camera.azimuthDeg = std::fmod(m_camera.azimuthDeg + delta * kOrbitSpeed, 360.0f);
        needsFrame = true;
    }

    const bool graphicsMissing = !m_rendererReady || !m_swapChain || !m_backBuffer;
    if (graphicsMissing || (m_loadState == LoadState::Ready && !m_sceneReady)) {
        // Retry with backoff (never stays blank for good).
        if (EnsureGraphics(false)) needsFrame = true;
    }
    if (needsFrame) RenderFrame();

    // "Open with": once, AFTER the model is shown, off the render path.
    if (m_previewing && modelShown && m_settings.showOpenWith && !m_openPath.empty()) {
        const UINT dpi = m_hwnd ? GetDpiForWindow(m_hwnd) : 0;
        const float dpiScale = (dpi ? static_cast<float>(dpi) / 96.0f : 1.0f) * m_renderScale;
        if (!m_openWithEnumerated) {
            EnsureOpenWith();
            m_dirty = true;
        } else {
            RefreshOpenWithIcons(dpiScale);
        }
    }
}

void PreviewWindow::EnsureOpenWith() {
    if (m_openWithEnumerated || m_openPath.empty()) return;
    m_openWithEnumerated = true;
    const wchar_t* extension = PathFindExtensionW(m_openPath.c_str());
    m_openWith = EnumerateOpenWithApps(extension);
    const UINT dpi = m_hwnd ? GetDpiForWindow(m_hwnd) : 0;
    const float dpiScale = (dpi ? static_cast<float>(dpi) / 96.0f : 1.0f) * m_renderScale;
    m_openWithIconPx = static_cast<int>(std::lround(kOpenWithIconDip * dpiScale));
    LoadOpenWithIcons(m_openWith.apps, m_openWithIconPx);
}

void PreviewWindow::RefreshOpenWithIcons(float dpiScale) {
    const int wanted = static_cast<int>(std::lround(kOpenWithIconDip * dpiScale));
    if (!m_openWithEnumerated || wanted == m_openWithIconPx || wanted <= 0) return;
    m_openWithIconPx = wanted;
    LoadOpenWithIcons(m_openWith.apps, wanted);
    m_dirty = true;
}

void PreviewWindow::FillOpenWithButtons() {
    m_info.openWith.clear();
    if (!m_settings.showOpenWith || !m_openWithEnumerated || m_openPath.empty()) return;
    const size_t appCount = m_openWith.apps.size();
    m_info.openWith.reserve(appCount + 1);
    for (size_t i = 0; i <= appCount; ++i) {
        OverlayButton button;
        if (i < appCount) {
            const OpenWithApp& app = m_openWith.apps[i];
            button.label = app.uiName;
            button.icon = app.icon;
            button.isDefault = app.isDefault;
        } else {
            button.isMore = true;   // programs not in the list: Windows' "Open with" dialog
        }
        const OverlayTarget self{OverlayHitKind::OpenWith, static_cast<int>(i)};
        button.hovered = m_hover == self && (!m_press.Valid() || m_press == self);
        button.pressed = m_press == self && !m_pressMoved && m_hover == self;
        m_info.openWith.push_back(std::move(button));
    }
}

OverlayTarget PreviewWindow::HitTest(POINT point) const {
    // Hit rects are in render pixels (client pixels × m_renderScale).
    const float x = (static_cast<float>(point.x) + 0.5f) * m_renderScale;
    const float y = (static_cast<float>(point.y) + 0.5f) * m_renderScale;
    // Back to front: the item drawn later (on top) wins.
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it) {
        const OverlayHitRect& hit = *it;
        if (x >= hit.left && x < hit.right && y >= hit.top && y < hit.bottom) return OverlayTarget{hit.kind, hit.button};
    }
    return OverlayTarget{};
}

namespace {
// Empty panel area is not clickable (it only swallows the click): no hand cursor.
bool IsClickable(const OverlayTarget& target) {
    return target.Valid() && target.kind != OverlayHitKind::Panel;
}

// The gear or part of the settings panel (the mouse doesn't rotate / pan / zoom here).
bool IsSettingsTarget(const OverlayTarget& target) {
    return target.Valid() && target.kind != OverlayHitKind::OpenWith;
}
}  // namespace

void PreviewWindow::UpdateHover(POINT point) {
    const OverlayTarget target = HitTest(point);
    if (target != m_hover) {
        m_hover = target;
        m_dirty = true;
        if (!m_dragging && !m_panning)
            SetCursor(LoadCursorW(nullptr, (IsClickable(target) || IsClickable(m_press)) ? IDC_HAND : IDC_ARROW));
    }
    if (!m_trackingLeave && m_hwnd) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = m_hwnd;
        m_trackingLeave = TrackMouseEvent(&track) != FALSE;
    }
}

void PreviewWindow::ActivateButton(int button) {
    if (button < 0 || m_openPath.empty() || ProcessIsLowIntegrity() || LaunchBlocked()) return;
    const size_t index = static_cast<size_t>(button);
    if (index < m_openWith.apps.size()) {
        const OpenWithApp& app = m_openWith.apps[index];
        LaunchRequest request;
        request.path = m_openPath;
        request.appName = app.name;
        request.appUiName = app.uiName;
        request.notify = m_hwnd;
        m_launchName = app.uiName;
        const bool started = StartLaunch(std::move(request));
        ShowToast((started ? L10n(L"Açılıyor: ", L"Opening: ") : L10n(L"Açılamadı: ", L"Could not open: ")) +
                  m_launchName);
    } else if (index == m_openWith.apps.size()) {
        g_lastLaunchEnd.store(GetTickCount64());   // a double click must not open a second dialog
        if (!OpenOpenWithDialogProcess(m_openPath))
            ShowToast(L10n(L"Açılamadı: Birlikte aç", L"Could not open the Open with dialog"));
    }
}

void PreviewWindow::SetSettingsOpen(bool open) {
    if (open == m_showSettings) return;
    m_showSettings = open;
    if (open) m_showHelp = false;   // the two panels are never open at once
    m_dirty = true;
}

void PreviewWindow::ActivateTarget(OverlayTarget target) {
    switch (target.kind) {
        case OverlayHitKind::OpenWith:
            ActivateButton(target.index);
            return;
        case OverlayHitKind::Gear:
            SetSettingsOpen(!m_showSettings);
            break;
        case OverlayHitKind::Toggle: {
            // Same path as the keyboard (save, toast, redraw).
            static constexpr KeyAction kToggleActions[] = {
                KeyAction::Grid,  KeyAction::BBox,       KeyAction::Plate, KeyAction::Overhang,    KeyAction::Edges,
                KeyAction::Wireframe, KeyAction::AutoRotate, KeyAction::Info, KeyAction::OpenWithBar,
            };
            static_assert(sizeof(kToggleActions) / sizeof(kToggleActions[0]) ==
                              static_cast<size_t>(OverlayToggle::Count),
                          "must match OverlayToggle");
            if (target.index < 0 || target.index >= static_cast<int>(OverlayToggle::Count)) return;
            const KeyAction action = kToggleActions[target.index];
            if (action == KeyAction::OpenWithBar && m_openPath.empty()) return;
            ApplyKey(action, ViewPreset::Isometric);
            break;
        }
        case OverlayHitKind::View: {
            static constexpr ViewPreset kPresets[] = {ViewPreset::Isometric, ViewPreset::Front, ViewPreset::Back,
                                                      ViewPreset::Left,      ViewPreset::Right, ViewPreset::Top,
                                                      ViewPreset::Bottom};
            if (target.index < 0 || target.index >= static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]))) return;
            ApplyKey(KeyAction::View, kPresets[target.index]);
            break;
        }
        case OverlayHitKind::CopySize:
            ApplyKey(KeyAction::Copy, ViewPreset::Isometric);
            break;
        case OverlayHitKind::Panel:
        case OverlayHitKind::None:
            return;
    }
    MarkInteraction();
}

void PreviewWindow::OpenWithDefaultProgram() {
    if (ProcessIsLowIntegrity()) return;   // nothing is launched from low integrity
    if (m_openPath.empty()) {
        ShowToast(L10n(L"Açılamadı: dosya yolu bilinmiyor", L"Could not open: file path unknown"));
        return;
    }
    if (LaunchBlocked()) return;
    EnsureOpenWith();
    std::wstring name;
    for (const OpenWithApp& app : m_openWith.apps) {
        if (app.isDefault) name = app.uiName;
    }
    if (name.empty()) name = m_openWith.defaultFriendlyName;
    if (name.empty()) name = L10n(L"varsayılan program", L"default program");
    // SAME path as double-click: the file's default verb (ShellExecuteEx, no verb given), on a separate thread
    // and with no owner window.
    LaunchRequest request;
    request.defaultProgram = true;
    request.path = m_openPath;
    request.notify = m_hwnd;
    m_launchName = name;
    const bool started = StartLaunch(std::move(request));
    ShowToast((started ? L10n(L"Açılıyor: ", L"Opening: ") : L10n(L"Açılamadı: ", L"Could not open: ")) + name);
}

void PreviewWindow::ResetCamera() {
    ApplyViewPreset(m_camera, ViewPreset::Isometric);
    m_rotateHold = false;
    m_settleFrames = kSettleFrames;
    m_dirty = true;
}

void PreviewWindow::MarkInteraction() {
    m_lastInput = GetTickCount64();
    m_settleFrames = kSettleFrames;
    m_dirty = true;
}

void PreviewWindow::ShowToast(std::wstring text) {
    m_toast = std::move(text);
    m_toastStart = GetTickCount64();
    m_dirty = true;
}

void PreviewWindow::EndMouseCapture() {
    if (!m_dragging && !m_panning && !m_press.Valid() && GetCapture() == m_hwnd) ReleaseCapture();
}

void PreviewWindow::ReloadSettings() {
    // M9: another preview window (same or another process) may have changed settings.
    const PreviewSettings settings = LoadPreviewSettings();
    if (std::memcmp(&settings, &m_settings, sizeof(settings)) != 0) {
        m_settings = settings;
        m_dirty = true;
    }
}

bool PreviewWindow::StartPreview() {
    if (!m_hwnd) {
        DebugLog(L"StartPreview: no window");
        return false;
    }
    ReloadSettings();
    ShowWindow(m_hwnd, SW_SHOW);
    m_previewing = true;
    m_lastInput = GetTickCount64();
    m_lastFrame = m_lastInput;
    m_settleFrames = kSettleFrames;
    m_dirty = true;
    // Set up graphics first so the "Loading…" frame shows right away; if that fails the timer retries.
    EnsureGraphics(true);
    if (!m_sceneReady && m_loadState != LoadState::Loading && m_loadState != LoadState::Failed) BeginLoad();
    m_timerInterval = 0;
    SetFrameTimer(kFrameIntervalMs);
    RenderFrame();
    return true;
}

void PreviewWindow::StopPreview() {
    m_previewing = false;
    if (m_hwnd) KillTimer(m_hwnd, kAnimationTimer);
    m_timerInterval = 0;
}

bool PreviewWindow::HandleKey(UINT message, WPARAM wParam, LPARAM lParam) {
    // Alt combinations (WM_SYSKEYDOWN) belong to Explorer.
    if (message != WM_KEYDOWN) return false;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (ctrl && wParam != 'C') return false;
    // If no model is shown (loading / error), keys go to the shell.
    if (m_loadState != LoadState::Ready) return false;

    KeyAction action = KeyAction::None;
    ViewPreset preset = ViewPreset::Isometric;
    auto view = [&](ViewPreset p) {
        action = KeyAction::View;
        preset = p;
    };
    switch (wParam) {
        case '0': case VK_NUMPAD0: view(ViewPreset::Isometric); break;
        case '1': case VK_NUMPAD1: view(ViewPreset::Front); break;
        case '2': case VK_NUMPAD2: view(ViewPreset::Back); break;
        case '3': case VK_NUMPAD3: view(ViewPreset::Left); break;
        case '4': case VK_NUMPAD4: view(ViewPreset::Right); break;
        case '5': case VK_NUMPAD5: view(ViewPreset::Top); break;
        case '6': case VK_NUMPAD6: view(ViewPreset::Bottom); break;
        case 'G': action = KeyAction::Grid; break;
        case 'B': action = KeyAction::BBox; break;
        case 'P': action = KeyAction::Plate; break;
        case 'O': action = KeyAction::Overhang; break;
        case 'W': action = KeyAction::Wireframe; break;
        case 'E': action = KeyAction::Edges; break;
        case 'R': action = KeyAction::AutoRotate; break;
        case 'I': action = KeyAction::Info; break;
        case 'H': action = KeyAction::Help; break;
        case 'C': action = KeyAction::Copy; break;
        // Ours only if the file path is known (the bar can be shown); Enter additionally only while the bar is on.
        // Otherwise the key goes to the frame (e.g. a file dialog's "Open" button). At low integrity
        // m_openPath is always empty: Enter launches nothing.
        case 'A':
            if (!m_openPath.empty()) action = KeyAction::OpenWithBar;
            break;
        case VK_RETURN:
            if (!m_openPath.empty() && m_settings.showOpenWith) action = KeyAction::OpenDefault;
            break;
        case VK_ESCAPE:
            if (m_showSettings) {
                action = KeyAction::CloseSettings;
            } else if (m_showHelp) {
                action = KeyAction::CloseHelp;
            }
            break;
        default: break;
    }
    if (action == KeyAction::None) return false;

    // Auto-repeat of a held key: the key is ours, but it is not applied again.
    const bool repeat = (lParam & (1 << 30)) != 0;
    if (!repeat) ApplyKey(action, preset);
    MarkInteraction();
    return true;
}

void PreviewWindow::ApplyKey(KeyAction action, ViewPreset preset) {
    // M9: only the changed value is written (settings changed by another window are not overwritten).
    auto toggle = [this](bool& value, const wchar_t* settingName, const wchar_t* label) {
        value = !value;
        SavePreviewSetting(settingName, value);
        ShowToast(std::wstring(label) + L": " + OnOff(value));
    };

    switch (action) {
        case KeyAction::View: {
            ApplyViewPreset(m_camera, preset);
            m_rotateHold = preset != ViewPreset::Isometric;
            const wchar_t* name = L10n(L"izometrik", L"isometric");
            switch (preset) {
                case ViewPreset::Isometric: name = L10n(L"izometrik", L"isometric"); break;
                case ViewPreset::Front: name = L10n(L"ön", L"front"); break;
                case ViewPreset::Back: name = L10n(L"arka", L"back"); break;
                case ViewPreset::Left: name = L10n(L"sol", L"left"); break;
                case ViewPreset::Right: name = L10n(L"sağ", L"right"); break;
                case ViewPreset::Top: name = L10n(L"üst", L"top"); break;
                case ViewPreset::Bottom: name = L10n(L"alt", L"bottom"); break;
            }
            ShowToast(std::wstring(L10n(L"Görünüm: ", L"View: ")) + name);
            break;
        }
        case KeyAction::Grid: toggle(m_settings.showGrid, kSettingShowGrid, L10n(L"Izgara", L"Grid")); break;
        case KeyAction::BBox:
            toggle(m_settings.showBBox, kSettingShowBBox, L10n(L"Sınır kutusu", L"Bounding box"));
            break;
        case KeyAction::Plate:
            toggle(m_settings.showPlate, kSettingShowPlate, L10n(L"K1 Max tablası", L"K1 Max build plate"));
            break;
        case KeyAction::Overhang:
            toggle(m_settings.overhang, kSettingOverhang, L10n(L"Askıda yüzeyler", L"Overhangs"));
            break;
        case KeyAction::Wireframe:
            toggle(m_settings.wireframe, kSettingWireframe, L10n(L"Tel kafes", L"Wireframe"));
            break;
        case KeyAction::Edges:
            toggle(m_settings.showEdges, kSettingShowEdges, L10n(L"Kenar çizgileri", L"Edge lines"));
            break;
        case KeyAction::AutoRotate:
            m_rotateHold = false;
            toggle(m_settings.autoRotate, kSettingAutoRotate, L10n(L"Otomatik döndürme", L"Auto-rotate"));
            break;
        case KeyAction::Info: toggle(m_settings.showInfo, kSettingShowInfo, L10n(L"Bilgi", L"Info text")); break;
        case KeyAction::Help:
            m_showHelp = !m_showHelp;
            if (m_showHelp) m_showSettings = false;   // the two panels are never open at once
            break;
        case KeyAction::CloseHelp: m_showHelp = false; break;
        case KeyAction::CloseSettings: SetSettingsOpen(false); break;
        case KeyAction::Copy: {
            if (m_loadState != LoadState::Ready) break;
            const std::wstring text = FormatSizeClipboardText(m_mesh);
            ShowToast(CopyTextToClipboard(m_hwnd, text)
                          ? L10n(L"Kopyalandı: ", L"Copied: ") + text
                          : std::wstring(L10n(L"Pano açılamadı", L"Could not open the clipboard")));
            break;
        }
        case KeyAction::OpenWithBar:
            toggle(m_settings.showOpenWith, kSettingShowOpenWith, L10n(L"Birlikte aç çubuğu", L"Open-with bar"));
            // Bar buttons disappear / move; panel items are added again in the next frame.
            m_hits.clear();
            m_hover = OverlayTarget{};
            if (m_press.kind == OverlayHitKind::OpenWith) m_press = OverlayTarget{};
            break;
        case KeyAction::OpenDefault: OpenWithDefaultProgram(); break;
        case KeyAction::None: break;
    }
}

LRESULT CALLBACK PreviewWindow::StaticWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* window = reinterpret_cast<PreviewWindow*>(create->lpCreateParams);

        window->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }

    auto* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);

    if (message == WM_NCDESTROY) {
        try {
            self->OnNcDestroy();
        } catch (...) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // The object may be released while a message is handled (e.g. Unload arriving during a key forwarded to the
    // frame): keep it alive until the call ends. For messages from inside the destructor lock() returns empty;
    // the object is still alive in that case.
    const std::shared_ptr<PreviewWindow> keepAlive = self->weak_from_this().lock();
    try {
        return self->WndProc(message, wParam, lParam);
    } catch (...) {
        // Exceptions must not cross the window procedure (kernel callback boundary). Only the local hwnd is
        // used: the animation is stopped and the message counts as handled.
        KillTimer(hwnd, kAnimationTimer);
        DebugLog(L"WndProc: exception caught, preview stopped");
        return 0;
    }
}

LRESULT PreviewWindow::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            m_occluded = false;
            PAINTSTRUCT paint{};
            BeginPaint(m_hwnd, &paint);
            if (m_previewing) {
                if (!m_rendererReady || !m_swapChain) EnsureGraphics(false);
                RenderFrame();
                // Restored from minimized / became visible: the animation timer speeds up again.
                if (IsPaneVisible()) SetFrameTimer(kFrameIntervalMs);
            }
            EndPaint(m_hwnd, &paint);
            return 0;
        }

        case WM_SIZE:
            m_occluded = false;
            if (wParam != SIZE_MINIMIZED) ResizeSwapChain();
            return 0;

        case WM_SHOWWINDOW:
            if (wParam && m_previewing) {
                m_dirty = true;
                SetFrameTimer(kFrameIntervalMs);
            }
            break;

        case WM_TIMER:
            if (wParam == kAnimationTimer) {
                OnTimer();
                return 0;
            }
            break;

        case kMsgLoadDone:
            OnLoadDone();
            return 0;

        case kMsgLaunchFailed:
            if (!m_launchName.empty()) ShowToast(L10n(L"Açılamadı: ", L"Could not open: ") + m_launchName);
            return 0;

        case WM_SETFOCUS:
            ReloadSettings();
            break;

        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;

        case WM_LBUTTONDOWN: {
            SetFocus(m_hwnd);
            m_ignoreDoubleClick = false;
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const OverlayTarget target = m_panning ? OverlayTarget{} : HitTest(point);
            if (m_showSettings && !IsSettingsTarget(target)) {
                // While the panel is open, a click outside only closes it (no rotation, no button).
                SetSettingsOpen(false);
                m_ignoreDoubleClick = true;
                return 0;
            }
            SetCapture(m_hwnd);
            if (target.Valid()) {
                // A button (Open with, gear, panel item) or empty panel area: no rotation; it is applied on
                // release (on the same item, without moving).
                m_press = target;
                m_pressPoint = point;
                m_pressMoved = false;
                m_hover = target;
                m_dirty = true;
                return 0;
            }
            m_dragging = true;
            m_lastMouse = point;
            MarkInteraction();
            return 0;
        }

        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            SetFocus(m_hwnd);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const OverlayTarget target = HitTest(point);
            if (m_showSettings && !IsSettingsTarget(target)) {
                SetSettingsOpen(false);
                m_pressMoved = true;
                return 0;
            }
            if (IsSettingsTarget(target)) {
                // No panning over the gear / panel.
                m_pressMoved = true;
                return 0;
            }
            SetCapture(m_hwnd);
            m_panning = true;
            m_pressMoved = true;   // another button while a button is pressed: click cancelled
            m_lastMouse.x = GET_X_LPARAM(lParam);
            m_lastMouse.y = GET_Y_LPARAM(lParam);
            MarkInteraction();
            return 0;
        }

        case WM_MOUSEMOVE: {
            const POINT current{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (m_press.Valid() && !m_panning) {
                // A drag that starts on a button does nothing (no rotate, no open).
                if (std::abs(current.x - m_pressPoint.x) > kClickSlopPx ||
                    std::abs(current.y - m_pressPoint.y) > kClickSlopPx) {
                    if (!m_pressMoved) m_dirty = true;
                    m_pressMoved = true;
                }
                UpdateHover(current);
                return 0;
            }
            if (!m_dragging && !m_panning) {
                UpdateHover(current);
                return 0;
            }
            const float dx = static_cast<float>(current.x - m_lastMouse.x);
            const float dy = static_cast<float>(current.y - m_lastMouse.y);
            if (m_dragging) {
                m_camera.azimuthDeg -= dx * kDragDegreesPerPixel;
                m_camera.elevationDeg =
                    std::clamp(m_camera.elevationDeg + dy * kDragDegreesPerPixel, -90.0f, 90.0f);
            } else {
                // pixelsPerMm is in render pixels; the mouse delta is in client pixels.
                const float pixelsPerMm =
                    std::max(m_renderer.LastFrame().pixelsPerMm / std::max(m_renderScale, 1e-4f), 1e-4f);
                m_camera.panX += dx / pixelsPerMm;
                m_camera.panY -= dy / pixelsPerMm;
                m_rotateHold = true;
            }
            m_lastMouse = current;
            MarkInteraction();
            return 0;
        }

        case WM_LBUTTONUP:
            if (m_press.Valid()) {
                const OverlayTarget pressed = m_press;
                const bool moved = m_pressMoved;
                const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                m_press = OverlayTarget{};
                m_pressMoved = false;
                EndMouseCapture();
                m_dirty = true;
                if (!moved && HitTest(point) == pressed) ActivateTarget(pressed);
                return 0;
            }
            if (m_dragging) {
                m_dragging = false;
                EndMouseCapture();
                MarkInteraction();
            }
            return 0;

        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            if (m_panning) {
                m_panning = false;
                EndMouseCapture();
                MarkInteraction();
            }
            return 0;

        case WM_CAPTURECHANGED:
            m_dragging = false;
            m_panning = false;
            if (m_press.Valid()) {
                m_press = OverlayTarget{};
                m_dirty = true;
            }
            return 0;

        case WM_MOUSELEAVE:
            m_trackingLeave = false;
            if (m_hover.Valid() && !m_press.Valid()) {
                m_hover = OverlayTarget{};
                m_dirty = true;
            }
            return 0;

        case WM_CONTEXTMENU:
            return 0;

        case WM_LBUTTONDBLCLK: {
            // Double click on a button: the second click is ignored (no second launch, no view reset).
            // The second click of a click that closed the panel from outside is ignored too.
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (m_ignoreDoubleClick) {
                m_ignoreDoubleClick = false;
                return 0;
            }
            const OverlayTarget target = HitTest(point);
            if (target.Valid()) return 0;
            if (m_showSettings) {
                SetSettingsOpen(false);
                return 0;
            }
            ResetCamera();
            MarkInteraction();
            return 0;
        }

        case WM_MOUSEWHEEL: {
            // The wheel doesn't zoom over the gear / panel.
            POINT wheelPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (ScreenToClient(m_hwnd, &wheelPoint) && IsSettingsTarget(HitTest(wheelPoint))) return 0;
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            m_camera.zoom = std::clamp(m_camera.zoom * std::pow(1.15f, static_cast<float>(delta) / 120.0f),
                                       kMinZoom, kMaxZoom);
            MarkInteraction();
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (HandleKey(message, wParam, lParam)) return 0;
            // Keys that aren't ours (Tab, arrows, F keys, Alt/Ctrl combinations) go to the shell.
            // The window (and this object) may be destroyed during the call: no member access afterwards.
            const HWND hwnd = m_hwnd;
            const ComPtr<IPreviewHandlerFrame> frame = m_frame;
            // If the frame sends the key back to us, don't forward it again (no infinite recursion).
            if (frame && !m_forwardingKey) {
                MSG msg{};
                msg.hwnd = hwnd;
                msg.message = message;
                msg.wParam = wParam;
                msg.lParam = lParam;
                msg.time = static_cast<DWORD>(GetMessageTime());
                const DWORD position = GetMessagePos();
                msg.pt.x = GET_X_LPARAM(position);
                msg.pt.y = GET_Y_LPARAM(position);
                const std::weak_ptr<PreviewWindow> weak = weak_from_this();
                m_forwardingKey = true;
                const HRESULT handled = frame->TranslateAccelerator(&msg);
                if (const std::shared_ptr<PreviewWindow> alive = weak.lock()) alive->m_forwardingKey = false;
                if (handled == S_OK) return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                const wchar_t* cursor = IDC_ARROW;
                if (m_dragging || m_panning) {
                    cursor = IDC_SIZEALL;
                } else if (IsClickable(m_hover) || IsClickable(m_press)) {
                    cursor = IDC_HAND;
                }
                SetCursor(LoadCursorW(nullptr, cursor));
                return TRUE;
            }
            break;
        }

        case WM_DESTROY:
            StopPreview();
            return 0;

        default:
            break;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------------------------
// PreviewHandler (COM). Every entry point catches all exceptions.
// ---------------------------------------------------------------------------------------------

PreviewHandler::PreviewHandler() = default;

PreviewHandler::~PreviewHandler() {
    try {
        if (m_window) {
            m_window->StopPreview();
            m_window->Destroy();
            m_window.reset();
        }
    } catch (...) {
    }
}

PreviewWindow& PreviewHandler::EnsureWindow() {
    if (!m_window) {
        m_window = std::make_shared<PreviewWindow>();
        m_window->SetFrame(m_frame);
    }
    return *m_window;
}

IFACEMETHODIMP PreviewHandler::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IPreviewHandler) {
        *ppv = static_cast<IPreviewHandler*>(this);
    } else if (riid == IID_IPreviewHandlerVisuals) {
        *ppv = static_cast<IPreviewHandlerVisuals*>(this);
    } else if (riid == IID_IInitializeWithFile) {
        *ppv = static_cast<IInitializeWithFile*>(this);
    } else if (riid == IID_IInitializeWithItem) {
        // IInitializeWithStream is deliberately NOT exposed: the shell tries the stream first and its name is
        // only the file name, so no path is found for "Open with" and the file is read from the stream (slower,
        // more memory). The shell falls back to IInitializeWithItem. Initialize(IStream*) stays for items
        // without a path.
        *ppv = static_cast<IInitializeWithItem*>(this);
    } else if (riid == IID_IObjectWithSite) {
        *ppv = static_cast<IObjectWithSite*>(this);
    } else if (riid == IID_IOleWindow) {
        *ppv = static_cast<IOleWindow*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) PreviewHandler::AddRef() {
    return static_cast<ULONG>(m_refs.fetch_add(1) + 1);
}

IFACEMETHODIMP_(ULONG) PreviewHandler::Release() {
    const LONG refs = m_refs.fetch_sub(1) - 1;
    if (refs == 0) delete this;
    return static_cast<ULONG>(refs);
}

IFACEMETHODIMP PreviewHandler::SetWindow(HWND hwndParent, const RECT* prc) {
    if (!prc) return E_INVALIDARG;
    try {
        // The pane window is created / moved with the parent window's DPI context.
        const ThreadDpiScope dpi(hwndParent);
        m_parent = hwndParent;
        m_rect = *prc;
        PreviewWindow& window = EnsureWindow();
        window.SetFrame(m_frame);
        if (!window.Create(hwndParent, *prc)) return E_FAIL;
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

IFACEMETHODIMP PreviewHandler::SetRect(const RECT* prc) {
    if (!prc) return E_INVALIDARG;
    try {
        m_rect = *prc;
        if (m_window) {
            const ThreadDpiScope dpi(m_window->Hwnd());
            m_window->SetRect(*prc);
        }
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

IFACEMETHODIMP PreviewHandler::DoPreview() {
    if (!m_window) return E_UNEXPECTED;
    // Incoming calls (while pumping messages) may release the handler and window: local references keep them alive.
    const ComPtr<IPreviewHandler> keepHandler(this);
    try {
        const std::shared_ptr<PreviewWindow> window = m_window;
        // Re-initialized after Unload but no SetWindow came: rebuild on the last parent window.
        if (!window->Hwnd() && m_parent && IsWindow(m_parent)) {
            const ThreadDpiScope parentDpi(m_parent);
            window->SetFrame(m_frame);
            if (!window->Create(m_parent, m_rect)) return E_FAIL;
        }
        const ThreadDpiScope dpi(window->Hwnd());
        // Returns immediately: the window shows the "Loading…" frame, the model loads on a separate thread.
        return window->StartPreview() ? S_OK : E_FAIL;
    } catch (...) {
        return E_FAIL;
    }
}

IFACEMETHODIMP PreviewHandler::Unload() {
    try {
        if (m_window) {
            const std::shared_ptr<PreviewWindow> window = std::move(m_window);
            m_window.reset();
            window->StopPreview();
            window->Destroy();
        }
        // M5: site and frame are NOT released here (SetSite(nullptr) / destructor).
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

IFACEMETHODIMP PreviewHandler::SetFocus() {
    try {
        if (!m_window) return E_UNEXPECTED;
        m_window->Focus();
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

IFACEMETHODIMP PreviewHandler::QueryFocus(HWND* phwnd) {
    if (!phwnd) return E_POINTER;
    try {
        *phwnd = ::GetFocus();
        return S_OK;
    } catch (...) {
        *phwnd = nullptr;
        return E_FAIL;
    }
}

IFACEMETHODIMP PreviewHandler::TranslateAccelerator(MSG*) {
    // Always S_FALSE: the message goes to the window normally; WndProc handles its own keys and forwards the
    // rest to the frame (IPreviewHandlerFrame::TranslateAccelerator) EXACTLY ONCE (recursion-guarded).
    return S_FALSE;
}

IFACEMETHODIMP PreviewHandler::SetBackgroundColor(COLORREF) {
    try {
        // Use the system theme instead of the shell's color (Explorer passes white even in light theme).
        if (m_window) m_window->SetBackgroundColor(SystemThemeBackgroundColor());
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

IFACEMETHODIMP PreviewHandler::SetFont(const LOGFONTW*) {
    return S_OK;
}

IFACEMETHODIMP PreviewHandler::SetTextColor(COLORREF) {
    return S_OK;
}

IFACEMETHODIMP PreviewHandler::Initialize(LPCWSTR filePath, DWORD) {
    if (!filePath || !*filePath) return E_INVALIDARG;
    try {
        EnsureWindow().SetSource(filePath, nullptr);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

IFACEMETHODIMP PreviewHandler::Initialize(IStream* stream, DWORD) {
    if (!stream) return E_INVALIDARG;
    try {
        EnsureWindow().SetSource(std::wstring(), stream);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

IFACEMETHODIMP PreviewHandler::Initialize(IShellItem* item, DWORD grfMode) {
    if (!item) return E_INVALIDARG;
    (void)grfMode;
    try {
        std::wstring filePath;
        LPWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) filePath = path;
        if (path) CoTaskMemFree(path);
        // Medium integrity, in-process: if there is a file system path the file is read directly from it (no
        // stream bound, no cross-process calls to Explorer, no handle held).
        if (!filePath.empty()) {
            const DWORD attributes = GetFileAttributesW(filePath.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                EnsureWindow().SetSource(filePath, nullptr);
                return S_OK;
            }
        }
        // No path (virtual folder, inside an archive) or unreadable: use the item's stream.
        ComPtr<IStream> stream;
        const HRESULT hr = item->BindToHandler(nullptr, BHID_Stream, IID_PPV_ARGS(stream.GetAddressOf()));
        if (FAILED(hr) || !stream) return FAILED(hr) ? hr : E_FAIL;
        EnsureWindow().SetSource(std::wstring(), stream, filePath);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

IFACEMETHODIMP PreviewHandler::SetSite(IUnknown* site) {
    try {
        m_site = site;
        m_frame.Reset();
        if (site) site->QueryInterface(IID_PPV_ARGS(m_frame.GetAddressOf()));
        if (m_window) m_window->SetFrame(m_frame);
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

IFACEMETHODIMP PreviewHandler::GetSite(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    try {
        if (!m_site) return E_FAIL;
        return m_site->QueryInterface(riid, ppv);
    } catch (...) {
        return E_FAIL;
    }
}

IFACEMETHODIMP PreviewHandler::GetWindow(HWND* phwnd) {
    if (!phwnd) return E_POINTER;
    try {
        *phwnd = m_window ? m_window->Hwnd() : nullptr;
        return S_OK;
    } catch (...) {
        *phwnd = nullptr;
        return E_FAIL;
    }
}

IFACEMETHODIMP PreviewHandler::ContextSensitiveHelp(BOOL) {
    return E_NOTIMPL;
}
