#include "thumbnail_provider.h"

#include "com_util.h"
#include "model.h"
#include "renderer.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace {
DirectX::XMFLOAT3 FromColorRef(COLORREF color) {
    return {GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f};
}

// Shared thumbnail renderer: a single Renderer on the shared D3D device (renderer.h), under SharedGpuMutex.
// After each request the model's GPU buffers and targets are released (ReleaseResources); only shaders
// and state objects remain.
struct GpuState {
    ComPtr<ID3D11Device> device;   // shared device reference (AcquireSharedGpuDevice)
    Renderer renderer;
    bool ready = false;
};

GpuState* g_gpu = new GpuState();   // leaked on purpose (destruction order at process exit)

constexpr uint32_t kThumbnailTriangleBudget = 1'500'000;
// More edge lines than this are unreadable in a thumbnail (they become a smudge); not uploaded to the GPU.
constexpr size_t kThumbnailMaxEdgeSegments = 300'000;

// Limit on the estimated total memory of thumbnails decoded at the same time. Explorer requests many
// thumbnails of a folder in parallel; each reads and decodes the whole file (a 487 MB STL takes ~0.8 GB). If
// the limit would be exceeded, the request waits until earlier ones finish. A file that alone exceeds the
// limit is still processed (but only on its own).
constexpr uint64_t kThumbnailLoadBudgetBytes = 1024ull * 1024ull * 1024ull;
std::mutex g_budgetMutex;
std::condition_variable g_budgetChanged;
uint64_t g_bytesInFlight = 0;

// Bytes read + the decoder's temporary data (measured: ~1.7 × file size).
uint64_t EstimateLoadBytes(uint64_t fileSize) {
    const uint64_t estimate = fileSize * 2 + 16ull * 1024ull * 1024ull;
    return std::min(estimate, kThumbnailLoadBudgetBytes);
}

class LoadBudgetScope {
public:
    explicit LoadBudgetScope(uint64_t bytes) : m_bytes(bytes) {
        std::unique_lock<std::mutex> lock(g_budgetMutex);
        g_budgetChanged.wait(lock, [this] {
            return g_bytesInFlight == 0 || g_bytesInFlight + m_bytes <= kThumbnailLoadBudgetBytes;
        });
        g_bytesInFlight += m_bytes;
    }
    ~LoadBudgetScope() {
        {
            std::lock_guard<std::mutex> lock(g_budgetMutex);
            g_bytesInFlight -= m_bytes;
        }
        g_budgetChanged.notify_all();
    }
    LoadBudgetScope(const LoadBudgetScope&) = delete;
    LoadBudgetScope& operator=(const LoadBudgetScope&) = delete;

private:
    uint64_t m_bytes;
};

// Called under SharedGpuMutex.
bool EnsureGpu() {
    if (g_gpu->ready) return true;
    if (!AcquireSharedGpuDevice(g_gpu->device)) return false;
    if (!g_gpu->renderer.Initialize(g_gpu->device.Get())) {
        g_gpu->renderer.Reset();
        g_gpu->device.Reset();
        ReleaseSharedGpuDevice();
        return false;
    }
    g_gpu->ready = true;
    return true;
}

// Called under SharedGpuMutex. If the device was lost (driver update, TDR) releases the shared state;
// the next thumbnail request acquires the device again.
void ResetGpuIfDeviceRemoved() {
    if (!g_gpu->device) return;
    const HRESULT reason = g_gpu->device->GetDeviceRemovedReason();
    if (reason == S_OK) return;

    wchar_t message[160] = {};
    _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE,
                 L"Direct3D device lost (0x%08lX), will be recreated",
                 static_cast<unsigned long>(reason));
    DebugLog(message);

    g_gpu->renderer.Reset();
    g_gpu->device.Reset();
    g_gpu->ready = false;
    ReleaseSharedGpuDevice();
}

// After every request (successful or not) releases the model's GPU buffers and targets.
class ReleaseGpuResourcesScope {
public:
    ReleaseGpuResourcesScope() = default;
    ~ReleaseGpuResourcesScope() {
        if (g_gpu->ready) g_gpu->renderer.ReleaseResources();
    }
    ReleaseGpuResourcesScope(const ReleaseGpuResourcesScope&) = delete;
    ReleaseGpuResourcesScope& operator=(const ReleaseGpuResourcesScope&) = delete;
};
}  // namespace

IFACEMETHODIMP ThumbnailProvider::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IThumbnailProvider) {
        *ppv = static_cast<IThumbnailProvider*>(this);
    } else if (riid == IID_IInitializeWithStream) {
        *ppv = static_cast<IInitializeWithStream*>(this);
    } else if (riid == IID_IInitializeWithFile) {
        *ppv = static_cast<IInitializeWithFile*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) ThumbnailProvider::AddRef() {
    return static_cast<ULONG>(m_refs.fetch_add(1) + 1);
}

IFACEMETHODIMP_(ULONG) ThumbnailProvider::Release() {
    const LONG refs = m_refs.fetch_sub(1) - 1;
    if (refs == 0) delete this;
    return static_cast<ULONG>(refs);
}

IFACEMETHODIMP ThumbnailProvider::Initialize(IStream* stream, DWORD) {
    if (!stream) return E_INVALIDARG;
    try {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        m_stream = stream;
        m_path.clear();
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

IFACEMETHODIMP ThumbnailProvider::Initialize(LPCWSTR filePath, DWORD) {
    if (!filePath || !*filePath) return E_INVALIDARG;
    try {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        m_path = filePath;
        m_stream.Reset();
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

void ThumbnailProvider::SnapshotSource(ComPtr<IStream>& stream, std::wstring& path, uint64_t& size) {
    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        stream = m_stream;
        path = m_path;
    }
    size = 0;
    if (stream) {
        STATSTG stat{};
        if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))) size = stat.cbSize.QuadPart;
    } else if (!path.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
}

IFACEMETHODIMP ThumbnailProvider::GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) {
    if (!phbmp || !pdwAlpha) return E_POINTER;
    *phbmp = nullptr;
    *pdwAlpha = WTSAT_ARGB;

    try {
        if (cx == 0) cx = 256;
        cx = std::clamp<UINT>(cx, 16, 1024);

        ComPtr<IStream> stream;
        std::wstring path;
        uint64_t sourceSize = 0;
        SnapshotSource(stream, path, sourceSize);
        if (!stream && path.empty()) return E_UNEXPECTED;

        // The model is decoded OUTSIDE the GPU lock (LoadModel is reentrant), but the total memory of concurrent
        // decodes is capped. The bytes read are released right after decoding (not cached).
        Mesh mesh;
        {
            const LoadBudgetScope budget(EstimateLoadBytes(sourceSize));
            std::vector<uint8_t> bytes;
            std::wstring error;
            bool read = false;
            if (stream) {
                LARGE_INTEGER zero{};
                stream->Seek(zero, STREAM_SEEK_SET, nullptr);   // there may be a second request on the same object
                read = ReadStreamAll(stream.Get(), bytes, error);
            } else {
                read = ReadFileAll(path.c_str(), bytes, error);
            }
            if (!read) {
                DebugLog(error.c_str());
                return E_FAIL;
            }
            // Thumbnails don't need the open-edge count; edge lines do make recesses/holes readable (vertex
            // welding cost measured: thumbnail of a 1.4M-triangle STL ~225 -> ~350 ms; above 3M triangles
            // the loader skips it anyway).
            const bool loaded =
                LoadModel(bytes.data(), bytes.size(), path.empty() ? nullptr : path.c_str(), kThumbnailTriangleBudget,
                          mesh, error, kModelLoadSkipTopology, kThumbnailLoadBudgetMs);
            std::vector<uint8_t>().swap(bytes);
            if (!loaded) {
                DebugLog(error.c_str());
                return E_FAIL;
            }
        }
        if (mesh.featureEdges.size() / 6 > kThumbnailMaxEdgeSegments) std::vector<float>().swap(mesh.featureEdges);

        std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
        if (!EnsureGpu()) {
            DebugLog(L"Could not create Direct3D device");
            return E_FAIL;
        }
        const ReleaseGpuResourcesScope releaseAfter;
        if (!g_gpu->renderer.SetMesh(mesh)) {
            ResetGpuIfDeviceRemoved();
            return E_FAIL;
        }
        // The CPU copy is on the GPU now; don't keep it in memory during readback and downscaling.
        mesh = Mesh{};

        const UINT renderSize = std::min<UINT>(cx * 2, 2048);

        // Default camera: isometric, Z up, model sits on the build plate.
        CameraState camera;
        camera.background = FromColorRef(SystemThemeBackgroundColor());
        const RenderOptions options = ThumbnailRenderOptions(cx);

        if (!g_gpu->renderer.Render(renderSize, renderSize, camera, options, 4)) {
            ResetGpuIfDeviceRemoved();
            return E_FAIL;
        }

        std::vector<uint8_t> pixels;
        UINT width = 0;
        UINT height = 0;
        if (!g_gpu->renderer.ReadPixels(pixels, width, height)) {
            ResetGpuIfDeviceRemoved();
            return E_FAIL;
        }

        std::vector<uint8_t> result;
        UINT resultWidth = 0;
        UINT resultHeight = 0;
        DownsampleBgraHalf(pixels, width, height, result, resultWidth, resultHeight);

        const HBITMAP bitmap = CreateHBitmapFromBgra(result, resultWidth, resultHeight);
        if (!bitmap) return E_FAIL;

        *phbmp = bitmap;
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}
