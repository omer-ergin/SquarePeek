#include "renderer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include <d3d11_4.h>

#include "com_util.h"

// Shaders are compiled with fxc at build time (src/shaders/*.hlsl -> build/shaders/<entry>.h);
// D3DCompile / d3dcompiler_47.dll is not used at run time.
#include "shader_VSMain.h"
#include "shader_PSMain.h"
#include "shader_VSLine.h"
#include "shader_PSLine.h"
#include "shader_VSEdge.h"
#include "shader_PSEdge.h"
#include "shader_VSFullscreen.h"
#include "shader_PSBackground.h"
#include "shader_PSBlit.h"

using Microsoft::WRL::ComPtr;

namespace {
struct alignas(16) CameraConstants {
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4 baseColor;
    DirectX::XMFLOAT4 params;
    DirectX::XMFLOAT4 params2;
};
static_assert(sizeof(CameraConstants) == 176, "CameraCB layout");

struct alignas(16) LineConstants {
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4 params;
};
static_assert(sizeof(LineConstants) == 80, "LineCB layout");

struct alignas(16) EdgeConstants {
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4 color;
    DirectX::XMFLOAT4 params;
};
static_assert(sizeof(EdgeConstants) == 96, "EdgeCB layout");

struct alignas(16) BackgroundConstants {
    DirectX::XMFLOAT4 topColor;
    DirectX::XMFLOAT4 bottomColor;
    DirectX::XMFLOAT4 params;
};

constexpr UINT kLineBufferCapacity = 24576;   // vertices
constexpr UINT kVerticesPerSegment = 6;

const DirectX::XMFLOAT4 kModelBaseColor{0.74f, 0.77f, 0.81f, 1.0f};
const DirectX::XMFLOAT4 kWireColor{0.13f, 0.15f, 0.19f, 1.0f};

template <typename T>
bool CreateConstantBuffer(ID3D11Device* device, ComPtr<ID3D11Buffer>& buffer) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(T);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(&desc, nullptr, buffer.ReleaseAndGetAddressOf()));
}

template <typename T>
void UpdateConstantBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const T& data) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &data, sizeof(T));
        context->Unmap(buffer, 0);
    }
}

float Distance3(float ax, float ay, float az, float bx, float by, float bz) {
    const float dx = ax - bx;
    const float dy = ay - by;
    const float dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Shared device state (under SharedGpuMutex).
struct SharedGpuState {
    ComPtr<ID3D11Device> device;
    long users = 0;
};

SharedGpuState& SharedGpu() {
    // Leaked on purpose: avoids destructor-order issues at process exit (driver DLLs may be unloaded first).
    static SharedGpuState* state = new SharedGpuState();
    return *state;
}
}  // namespace

std::recursive_mutex& SharedGpuMutex() {
    static std::recursive_mutex* mutex = new std::recursive_mutex();
    return *mutex;
}

bool AcquireSharedGpuDevice(ComPtr<ID3D11Device>& device) {
    device.Reset();
    std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
    SharedGpuState& state = SharedGpu();
    if (state.device && state.device->GetDeviceRemovedReason() != S_OK) {
        DebugLog(L"Shared Direct3D device lost, re-creating");
        state.device.Reset();
    }
    if (!state.device) {
        ComPtr<ID3D11DeviceContext> context;
        if (!CreateD3D11Device(state.device.GetAddressOf(), context.GetAddressOf())) {
            state.device.Reset();
            return false;
        }
        // The context is still guarded by SharedGpuMutex; this is extra safety for D2D's and DXGI's internal use.
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    }
    device = state.device;
    ++state.users;
    return true;
}

void ReleaseSharedGpuDevice() {
    std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
    SharedGpuState& state = SharedGpu();
    if (state.users > 0 && --state.users == 0 && state.device) {
        // The device is intentionally NOT released: destroying a D3D11 device does not give back driver event handles
        // (measured: ~7 Events per device). An idle device holds almost no memory (model buffers, targets and swap
        // chains are released with windows / thumbnail requests); if it is lost, it is re-created on the next request.
        ComPtr<ID3D11DeviceContext> context;
        state.device->GetImmediateContext(context.GetAddressOf());
        if (context) {
            context->ClearState();
            context->Flush();
        }
    }
}

long SharedGpuDeviceUsers() {
    std::lock_guard<std::recursive_mutex> lock(SharedGpuMutex());
    return SharedGpu().users;
}

bool Renderer::Initialize(ID3D11Device* device) {
    if (!device) return false;
    Reset();
    m_device = device;
    m_device->GetImmediateContext(m_context.GetAddressOf());
    if (!m_context) {
        m_device.Reset();
        return false;
    }
    return CreateDeviceContextPieces();
}

void Renderer::ReleaseResources() {
    ReleaseTargets();
    m_staging.Reset();
    m_stagingWidth = 0;
    m_stagingHeight = 0;
    m_vertexBuffer.Reset();
    m_vertexCount = 0;
    m_edgeBuffer.Reset();
    m_edgeVertexCount = 0;
    m_edgeLengthMm = 0.0;
    m_radius = 1.0f;
    for (float& b : m_bounds) b = 0.0f;
    m_fit = FitFrame{};
    m_fitValid = false;
    std::vector<DirectX::XMFLOAT3>().swap(m_fitPoints);
    m_frame = FrameInfo{};
    // D3D11 actually frees released buffers only if they are not bound to the context and after a Flush (shared
    // context: every frame sets up its state from scratch).
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
}

void Renderer::Reset() {
    ReleaseResources();
    m_msaaQueried = 0;
    m_msaaEffective = 1;

    m_modelVs.Reset();
    m_modelPs.Reset();
    m_layout.Reset();
    m_fullscreenVs.Reset();
    m_backgroundPs.Reset();
    m_blitPs.Reset();
    m_cameraCb.Reset();
    m_backgroundCb.Reset();
    m_depthOn.Reset();
    m_depthOff.Reset();
    m_rasterizer.Reset();
    m_sampler.Reset();

    m_lineVs.Reset();
    m_linePs.Reset();
    m_lineLayout.Reset();
    m_lineCb.Reset();
    m_lineVb.Reset();
    m_depthLessEqual.Reset();
    m_depthReadOnly.Reset();
    m_wireRasterizer.Reset();
    m_blendAlpha.Reset();

    m_edgeVs.Reset();
    m_edgePs.Reset();
    m_edgeLayout.Reset();
    m_edgeCb.Reset();
    m_edgeBuffer.Reset();
    m_edgeVertexCount = 0;

    m_lineScratch.clear();
    m_lineFine.clear();
    m_lineNormal.clear();
    m_lineMain.clear();
    m_lineZero.clear();
    m_linePlate.clear();
    m_lineBox.clear();

    m_context.Reset();
    m_device.Reset();
}

bool Renderer::CreateDeviceContextPieces() {
    // --- model ---
    if (FAILED(m_device->CreateVertexShader(g_VSMain, sizeof(g_VSMain), nullptr, m_modelVs.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(m_device->CreatePixelShader(g_PSMain, sizeof(g_PSMain), nullptr, m_modelPs.ReleaseAndGetAddressOf())))
        return false;

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(m_device->CreateInputLayout(layout, ARRAYSIZE(layout), g_VSMain, sizeof(g_VSMain),
                                           m_layout.ReleaseAndGetAddressOf())))
        return false;

    // --- lines ---
    if (FAILED(m_device->CreateVertexShader(g_VSLine, sizeof(g_VSLine), nullptr, m_lineVs.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(m_device->CreatePixelShader(g_PSLine, sizeof(g_PSLine), nullptr, m_linePs.ReleaseAndGetAddressOf())))
        return false;

    static_assert(sizeof(LineVertex) == 48, "LineVertex must be 48 bytes");
    const D3D11_INPUT_ELEMENT_DESC lineLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(m_device->CreateInputLayout(lineLayout, ARRAYSIZE(lineLayout), g_VSLine, sizeof(g_VSLine),
                                           m_lineLayout.ReleaseAndGetAddressOf())))
        return false;

    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = kLineBufferCapacity * static_cast<UINT>(sizeof(LineVertex));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, m_lineVb.ReleaseAndGetAddressOf()))) return false;
    }

    // --- feature edges ---
    if (FAILED(m_device->CreateVertexShader(g_VSEdge, sizeof(g_VSEdge), nullptr, m_edgeVs.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(m_device->CreatePixelShader(g_PSEdge, sizeof(g_PSEdge), nullptr, m_edgePs.ReleaseAndGetAddressOf())))
        return false;
    const D3D11_INPUT_ELEMENT_DESC edgeLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(m_device->CreateInputLayout(edgeLayout, ARRAYSIZE(edgeLayout), g_VSEdge, sizeof(g_VSEdge),
                                           m_edgeLayout.ReleaseAndGetAddressOf())))
        return false;

    // --- full screen (background + copy) ---
    if (FAILED(m_device->CreateVertexShader(g_VSFullscreen, sizeof(g_VSFullscreen), nullptr,
                                            m_fullscreenVs.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(m_device->CreatePixelShader(g_PSBackground, sizeof(g_PSBackground), nullptr,
                                           m_backgroundPs.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(m_device->CreatePixelShader(g_PSBlit, sizeof(g_PSBlit), nullptr, m_blitPs.ReleaseAndGetAddressOf())))
        return false;

    if (!CreateConstantBuffer<CameraConstants>(m_device.Get(), m_cameraCb)) return false;
    if (!CreateConstantBuffer<BackgroundConstants>(m_device.Get(), m_backgroundCb)) return false;
    if (!CreateConstantBuffer<LineConstants>(m_device.Get(), m_lineCb)) return false;
    if (!CreateConstantBuffer<EdgeConstants>(m_device.Get(), m_edgeCb)) return false;

    // --- depth states ---
    D3D11_DEPTH_STENCIL_DESC depthOnDesc{};
    depthOnDesc.DepthEnable = TRUE;
    depthOnDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthOnDesc.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(m_device->CreateDepthStencilState(&depthOnDesc, m_depthOn.ReleaseAndGetAddressOf()))) return false;

    D3D11_DEPTH_STENCIL_DESC depthOffDesc{};
    depthOffDesc.DepthEnable = FALSE;
    depthOffDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthOffDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(m_device->CreateDepthStencilState(&depthOffDesc, m_depthOff.ReleaseAndGetAddressOf()))) return false;

    D3D11_DEPTH_STENCIL_DESC lessEqualDesc{};
    lessEqualDesc.DepthEnable = TRUE;
    lessEqualDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    lessEqualDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(m_device->CreateDepthStencilState(&lessEqualDesc, m_depthLessEqual.ReleaseAndGetAddressOf())))
        return false;

    D3D11_DEPTH_STENCIL_DESC readOnlyDesc{};
    readOnlyDesc.DepthEnable = TRUE;
    readOnlyDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    readOnlyDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(m_device->CreateDepthStencilState(&readOnlyDesc, m_depthReadOnly.ReleaseAndGetAddressOf())))
        return false;

    // --- rasterizer states ---
    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.MultisampleEnable = TRUE;
    if (FAILED(m_device->CreateRasterizerState(&rasterizerDesc, m_rasterizer.ReleaseAndGetAddressOf())))
        return false;

    D3D11_RASTERIZER_DESC wireDesc{};
    wireDesc.FillMode = D3D11_FILL_WIREFRAME;
    wireDesc.CullMode = D3D11_CULL_NONE;
    wireDesc.FrontCounterClockwise = FALSE;
    wireDesc.DepthBias = -200;
    wireDesc.DepthBiasClamp = 0.0f;
    wireDesc.SlopeScaledDepthBias = -1.0f;
    wireDesc.DepthClipEnable = TRUE;
    wireDesc.MultisampleEnable = TRUE;
    if (FAILED(m_device->CreateRasterizerState(&wireDesc, m_wireRasterizer.ReleaseAndGetAddressOf())))
        return false;

    // --- blending (lines) ---
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_device->CreateBlendState(&blendDesc, m_blendAlpha.ReleaseAndGetAddressOf()))) return false;

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateSamplerState(&samplerDesc, m_sampler.ReleaseAndGetAddressOf()))) return false;

    m_lineScratch.reserve(kLineBufferCapacity);
    return true;
}

bool Renderer::SetMesh(const Mesh& mesh) {
    m_vertexBuffer.Reset();
    m_vertexCount = 0;
    m_edgeBuffer.Reset();
    m_edgeVertexCount = 0;
    m_edgeLengthMm = 0.0;
    m_radius = mesh.radius > 1e-6f ? mesh.radius : 1.0f;
    m_fit = FitFrame{};
    m_fitValid = false;
    m_frame = FrameInfo{};
    for (int i = 0; i < 6; ++i) m_bounds[i] = mesh.bounds[i];

    m_fitPoints.clear();
    const size_t vertexCount = mesh.vertices.size() / 6;
    const size_t maxPoints = 20000;
    const size_t step = vertexCount > maxPoints ? (vertexCount + maxPoints - 1) / maxPoints : 1;
    try {
        m_fitPoints.reserve(vertexCount / step + 1);
        for (size_t v = 0; v < vertexCount; v += step) {
            m_fitPoints.push_back({mesh.vertices[v * 6 + 0], mesh.vertices[v * 6 + 1], mesh.vertices[v * 6 + 2]});
        }
    } catch (...) {
        m_fitPoints.clear();   // fit falls back to the model radius
    }

    if (!m_device || vertexCount == 0) return false;
    const size_t bytes = vertexCount * 6 * sizeof(float);
    if (bytes > 0xFFFFFFFFull) return false;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(bytes);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = mesh.vertices.data();

    if (FAILED(m_device->CreateBuffer(&desc, &data, m_vertexBuffer.GetAddressOf()))) {
        m_vertexBuffer.Reset();
        return false;
    }
    m_vertexCount = static_cast<UINT>(vertexCount);

    // Feature edges (optional): if they cannot be created the model is drawn without edges and SetMesh still succeeds.
    const size_t edgeVertices = (mesh.featureEdges.size() / 6) * 2;
    const size_t edgeBytes = edgeVertices * 3 * sizeof(float);
    if (edgeVertices > 0 && edgeBytes <= 0xFFFFFFFFull) {
        D3D11_BUFFER_DESC edgeDesc{};
        edgeDesc.ByteWidth = static_cast<UINT>(edgeBytes);
        edgeDesc.Usage = D3D11_USAGE_IMMUTABLE;
        edgeDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA edgeData{};
        edgeData.pSysMem = mesh.featureEdges.data();
        if (SUCCEEDED(m_device->CreateBuffer(&edgeDesc, &edgeData, m_edgeBuffer.GetAddressOf()))) {
            m_edgeVertexCount = static_cast<UINT>(edgeVertices);
            const float* e = mesh.featureEdges.data();
            for (size_t s = 0; s < edgeVertices / 2; ++s, e += 6) {
                m_edgeLengthMm += Distance3(e[0], e[1], e[2], e[3], e[4], e[5]);
            }
        } else {
            m_edgeBuffer.Reset();
        }
    }
    return true;
}

void Renderer::ReleaseTargets() {
    m_targets = Targets{};
    m_width = 0;
    m_height = 0;
    m_samples = 0;
}

bool Renderer::EnsureTargets(UINT width, UINT height, UINT samples) {
    if (!m_device) return false;
    if (width == 0 || height == 0) return false;
    if (samples == 0) samples = 1;

    UINT effectiveSamples = samples;
    if (effectiveSamples > 1) {
        // Constant per device: not queried every frame.
        if (m_msaaQueried != samples) {
            UINT quality = 0;
            m_msaaQueried = samples;
            m_msaaEffective =
                (FAILED(m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, samples, &quality)) ||
                 quality == 0)
                    ? 1
                    : samples;
        }
        effectiveSamples = m_msaaEffective;
    }

    if (m_targets.color && width == m_width && height == m_height && effectiveSamples == m_samples) return true;

    ReleaseTargets();

    auto create = [&](UINT sampleCount) -> bool {
        D3D11_TEXTURE2D_DESC colorDesc{};
        colorDesc.Width = width;
        colorDesc.Height = height;
        colorDesc.MipLevels = 1;
        colorDesc.ArraySize = 1;
        colorDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        colorDesc.SampleDesc.Count = sampleCount;
        colorDesc.Usage = D3D11_USAGE_DEFAULT;
        colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (sampleCount == 1) colorDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(m_device->CreateTexture2D(&colorDesc, nullptr, m_targets.color.GetAddressOf()))) {
            ReleaseTargets();
            return false;
        }
        if (FAILED(m_device->CreateRenderTargetView(m_targets.color.Get(), nullptr,
                                                    m_targets.colorRtv.GetAddressOf()))) {
            ReleaseTargets();
            return false;
        }

        D3D11_TEXTURE2D_DESC depthDesc = colorDesc;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(m_device->CreateTexture2D(&depthDesc, nullptr, m_targets.depth.GetAddressOf()))) {
            ReleaseTargets();
            return false;
        }
        if (FAILED(m_device->CreateDepthStencilView(m_targets.depth.Get(), nullptr,
                                                    m_targets.depthDsv.GetAddressOf()))) {
            ReleaseTargets();
            return false;
        }

        if (sampleCount > 1) {
            D3D11_TEXTURE2D_DESC resolvedDesc = colorDesc;
            resolvedDesc.SampleDesc.Count = 1;
            resolvedDesc.SampleDesc.Quality = 0;
            // The RT flag is also needed so the overlay (Direct2D) can draw to this texture as a target.
            resolvedDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(m_device->CreateTexture2D(&resolvedDesc, nullptr, m_targets.resolved.GetAddressOf()))) {
                ReleaseTargets();
                return false;
            }
            if (FAILED(m_device->CreateShaderResourceView(m_targets.resolved.Get(), nullptr,
                                                          m_targets.resolvedSrv.GetAddressOf()))) {
                ReleaseTargets();
                return false;
            }
        } else {
            m_targets.resolved = m_targets.color;
            if (FAILED(m_device->CreateShaderResourceView(m_targets.resolved.Get(), nullptr,
                                                          m_targets.resolvedSrv.GetAddressOf()))) {
                ReleaseTargets();
                return false;
            }
        }

        m_width = width;
        m_height = height;
        m_samples = sampleCount;
        return true;
    };

    if (create(effectiveSamples)) return true;
    if (effectiveSamples > 1) return create(1);
    return false;
}

Renderer::FitFrame Renderer::ComputeFit(const DirectX::XMMATRIX& view, float aspect, bool includePlate,
                                        bool includeBBox) const {
    FitFrame frame;
    if (m_fitPoints.empty() && !includePlate && !includeBBox) {
        frame.halfBase = m_radius;
        return frame;
    }

    float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
    const auto add = [&](float x, float y, float z) {
        DirectX::XMFLOAT3 projected{};
        DirectX::XMStoreFloat3(&projected,
                               DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(x, y, z, 1.0f), view));
        minX = std::min(minX, projected.x);
        maxX = std::max(maxX, projected.x);
        minY = std::min(minY, projected.y);
        maxY = std::max(maxY, projected.y);
    };
    if (m_fitPoints.empty() || includeBBox) {
        // If the bounding box is drawn, fit its corners too (so lines and X/Y/Z labels stay inside the window);
        // if there are no model points (memory), these are used instead of the model.
        for (int i = 0; i < 8; ++i) add(m_bounds[(i & 1) ? 3 : 0], m_bounds[(i & 2) ? 4 : 1], m_bounds[(i & 4) ? 5 : 2]);
    }
    for (const DirectX::XMFLOAT3& source : m_fitPoints) add(source.x, source.y, source.z);
    if (includePlate) {
        // With the K1 Max plate on, fit the plate's 4 corners too (z = 0 plane).
        const float h = kPlateSizeMm * 0.5f;
        add(-h, -h, 0.0f);
        add(+h, -h, 0.0f);
        add(+h, +h, 0.0f);
        add(-h, +h, 0.0f);
    }

    frame.centerX = (minX + maxX) * 0.5f;
    frame.centerY = (minY + maxY) * 0.5f;
    const float extentX = (maxX - minX) * 0.5f;
    const float extentY = (maxY - minY) * 0.5f;
    frame.halfBase = std::max(extentY, extentX / std::max(aspect, 0.01f)) * 1.10f;
    if (!(frame.halfBase > 1e-6f) || !std::isfinite(frame.halfBase) || !std::isfinite(frame.centerX) ||
        !std::isfinite(frame.centerY)) {
        // Degenerate fit box (single point / line): use the model radius.
        if (!std::isfinite(frame.centerX) || !std::isfinite(frame.centerY)) {
            frame.centerX = 0.0f;
            frame.centerY = 0.0f;
        }
        frame.halfBase = m_radius;
    }
    return frame;
}

void Renderer::AppendSegment(std::vector<LineVertex>& out, const DirectX::XMFLOAT3& p, const DirectX::XMFLOAT3& q,
                             const DirectX::XMFLOAT4& color, float widthPx) {
    out.push_back({p, q, color, {+1.0f, widthPx}});
    out.push_back({p, q, color, {-1.0f, widthPx}});
    out.push_back({q, p, color, {-1.0f, widthPx}});
    out.push_back({q, p, color, {-1.0f, widthPx}});
    out.push_back({p, q, color, {-1.0f, widthPx}});
    out.push_back({q, p, color, {+1.0f, widthPx}});
}

Renderer::LineRanges Renderer::BuildLines(const RenderOptions& options, bool dark, const GridLayout& grid, float zGrid,
                                          float pixelsPerMm, FrameInfo& frame) {
    using DirectX::XMFLOAT3;
    using DirectX::XMFLOAT4;

    const float s = std::clamp(options.lineScale, 0.5f, 4.0f);
    auto widthOf = [s](float w) { return std::max(1.0f, w * s); };

    const XMFLOAT4 fineColor = dark ? XMFLOAT4{1.0f, 1.0f, 1.0f, 0.06f} : XMFLOAT4{0.0f, 0.0f, 0.0f, 0.07f};
    const XMFLOAT4 normalColor = dark ? XMFLOAT4{1.0f, 1.0f, 1.0f, 0.14f} : XMFLOAT4{0.0f, 0.0f, 0.0f, 0.16f};
    const XMFLOAT4 strongColor = dark ? XMFLOAT4{1.0f, 1.0f, 1.0f, 0.34f} : XMFLOAT4{0.0f, 0.0f, 0.0f, 0.36f};
    const XMFLOAT4 borderColor = strongColor;
    const XMFLOAT4 zeroXColor{0.86f, 0.24f, 0.22f, 0.95f};   // along the X axis (y = originY)
    const XMFLOAT4 zeroYColor{0.20f, 0.66f, 0.30f, 0.95f};   // along the Y axis (x = originX)
    const XMFLOAT4 plateColor = dark ? XMFLOAT4{0.40f, 0.68f, 1.0f, 0.95f} : XMFLOAT4{0.10f, 0.42f, 0.92f, 0.95f};
    const XMFLOAT4 boxColor = dark ? XMFLOAT4{0.55f, 0.80f, 1.0f, 0.90f} : XMFLOAT4{0.08f, 0.30f, 0.70f, 0.90f};

    const float wFine = widthOf(1.0f);
    const float wNormal = widthOf(1.0f);
    const float wStrong = widthOf(1.4f);
    const float wBorder = widthOf(1.4f);
    const float wZero = widthOf(2.0f);
    const float wPlate = widthOf(2.0f);
    const float wBox = widthOf(1.5f);

    m_lineScratch.clear();
    m_lineFine.clear();
    m_lineNormal.clear();
    m_lineMain.clear();
    m_lineZero.clear();
    m_linePlate.clear();
    m_lineBox.clear();

    frame.drawnStepMm = 0.0f;
    frame.fineGridDrawn = false;

    bool drawNormal = false;
    bool drawStrong = false;
    if (options.showGrid) {
        const float pxStep = grid.stepMm * pixelsPerMm;
        const int strongEvery = std::max(grid.strongEvery, 1);
        drawNormal = pxStep >= 5.0f * s;
        drawStrong = pxStep * static_cast<float>(strongEvery) >= 5.0f * s;
        if (drawNormal) {
            frame.drawnStepMm = grid.stepMm;
        } else if (drawStrong) {
            frame.drawnStepMm = grid.stepMm * static_cast<float>(strongEvery);
        } else {
            frame.drawnStepMm = 0.0f;
        }

        // Constant-x lines (run along Y).
        for (int k = -grid.lowX; k <= grid.highX; ++k) {
            const float x = grid.originX + static_cast<float>(k) * grid.stepMm;
            const XMFLOAT3 p{x, grid.minY, zGrid};
            const XMFLOAT3 q{x, grid.maxY, zGrid};
            if (k == 0) {
                AppendSegment(m_lineZero, p, q, zeroYColor, wZero);
            } else if (k == -grid.lowX || k == grid.highX) {
                AppendSegment(m_lineMain, p, q, borderColor, wBorder);
            } else if (k % strongEvery == 0) {
                if (drawStrong) AppendSegment(m_lineMain, p, q, strongColor, wStrong);
            } else if (drawNormal) {
                AppendSegment(m_lineNormal, p, q, normalColor, wNormal);
            }
        }
        // Constant-y lines (run along X).
        for (int k = -grid.lowY; k <= grid.highY; ++k) {
            const float y = grid.originY + static_cast<float>(k) * grid.stepMm;
            const XMFLOAT3 p{grid.minX, y, zGrid};
            const XMFLOAT3 q{grid.maxX, y, zGrid};
            if (k == 0) {
                AppendSegment(m_lineZero, p, q, zeroXColor, wZero);
            } else if (k == -grid.lowY || k == grid.highY) {
                AppendSegment(m_lineMain, p, q, borderColor, wBorder);
            } else if (k % strongEvery == 0) {
                if (drawStrong) AppendSegment(m_lineMain, p, q, strongColor, wStrong);
            } else if (drawNormal) {
                AppendSegment(m_lineNormal, p, q, normalColor, wNormal);
            }
        }

        if (options.fineGrid && FineGridVisible(grid, pixelsPerMm, s)) {
            for (int j = -grid.lowX * 10; j <= grid.highX * 10; ++j) {
                if (j % 10 == 0) continue;
                const float x = grid.originX + static_cast<float>(j) * grid.fineStepMm;
                AppendSegment(m_lineFine, {x, grid.minY, zGrid}, {x, grid.maxY, zGrid}, fineColor, wFine);
            }
            for (int j = -grid.lowY * 10; j <= grid.highY * 10; ++j) {
                if (j % 10 == 0) continue;
                const float y = grid.originY + static_cast<float>(j) * grid.fineStepMm;
                AppendSegment(m_lineFine, {grid.minX, y, zGrid}, {grid.maxX, y, zGrid}, fineColor, wFine);
            }
            frame.fineGridDrawn = true;
        }
    }

    if (options.showPlate) {
        const float h = kPlateSizeMm * 0.5f;
        const XMFLOAT3 c0{-h, -h, zGrid};
        const XMFLOAT3 c1{+h, -h, zGrid};
        const XMFLOAT3 c2{+h, +h, zGrid};
        const XMFLOAT3 c3{-h, +h, zGrid};
        AppendSegment(m_linePlate, c0, c1, plateColor, wPlate);
        AppendSegment(m_linePlate, c1, c2, plateColor, wPlate);
        AppendSegment(m_linePlate, c2, c3, plateColor, wPlate);
        AppendSegment(m_linePlate, c3, c0, plateColor, wPlate);
    }

    if (options.showBBox) {
        const float x0 = m_bounds[0], y0 = m_bounds[1], z0 = m_bounds[2];
        const float x1 = m_bounds[3], y1 = m_bounds[4], z1 = m_bounds[5];
        const XMFLOAT3 corners[8] = {
            {x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
            {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1},
        };
        const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                  {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : edges) {
            AppendSegment(m_lineBox, corners[edge[0]], corners[edge[1]], boxColor, wBox);
        }
    }

    // Capacity: fine lines are dropped first, then normal lines.
    auto total = [this]() {
        return m_lineFine.size() + m_lineNormal.size() + m_lineMain.size() + m_lineZero.size() + m_linePlate.size() +
               m_lineBox.size();
    };
    if (total() > kLineBufferCapacity && !m_lineFine.empty()) {
        m_lineFine.clear();
        frame.fineGridDrawn = false;
    }
    if (total() > kLineBufferCapacity && !m_lineNormal.empty()) {
        m_lineNormal.clear();
        frame.drawnStepMm = drawStrong ? grid.stepMm * static_cast<float>(std::max(grid.strongEvery, 1)) : 0.0f;
    }
    if (total() > kLineBufferCapacity) {
        // Does not happen in practice (grid is limited to ~200 lines per direction); still, clip the strong lines.
        const size_t others = m_lineZero.size() + m_linePlate.size() + m_lineBox.size();
        size_t room = others < kLineBufferCapacity ? kLineBufferCapacity - others : 0;
        room -= room % kVerticesPerSegment;
        if (m_lineMain.size() > room) m_lineMain.resize(room);
        if (total() > kLineBufferCapacity) {
            m_lineMain.clear();
            m_linePlate.clear();
            if (total() > kLineBufferCapacity) m_lineZero.clear();
        }
    }

    // Faintest to strongest: blend order matters (depth is not written).
    LineRanges ranges;
    ranges.gridStart = 0;
    m_lineScratch.insert(m_lineScratch.end(), m_lineFine.begin(), m_lineFine.end());
    m_lineScratch.insert(m_lineScratch.end(), m_lineNormal.begin(), m_lineNormal.end());
    m_lineScratch.insert(m_lineScratch.end(), m_lineMain.begin(), m_lineMain.end());
    m_lineScratch.insert(m_lineScratch.end(), m_lineZero.begin(), m_lineZero.end());
    m_lineScratch.insert(m_lineScratch.end(), m_linePlate.begin(), m_linePlate.end());
    ranges.gridCount = static_cast<UINT>(m_lineScratch.size());
    ranges.bboxStart = ranges.gridCount;
    m_lineScratch.insert(m_lineScratch.end(), m_lineBox.begin(), m_lineBox.end());
    ranges.bboxCount = static_cast<UINT>(m_lineScratch.size()) - ranges.bboxStart;
    return ranges;
}

void Renderer::DrawLines(const LineRanges& ranges, const DirectX::XMFLOAT4X4& viewProj, UINT width, UINT height) {
    const UINT count = ranges.gridCount + ranges.bboxCount;
    if (count == 0 || !m_lineVb || count > kLineBufferCapacity || m_lineScratch.size() < count) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_lineVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, m_lineScratch.data(), static_cast<size_t>(count) * sizeof(LineVertex));
    m_context->Unmap(m_lineVb.Get(), 0);

    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_context->OMSetBlendState(m_blendAlpha.Get(), blendFactor, 0xFFFFFFFFu);
    m_context->RSSetState(m_rasterizer.Get());
    m_context->IASetInputLayout(m_lineLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = static_cast<UINT>(sizeof(LineVertex));
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffer = m_lineVb.Get();
    m_context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    m_context->VSSetShader(m_lineVs.Get(), nullptr, 0);
    m_context->PSSetShader(m_linePs.Get(), nullptr, 0);
    ID3D11Buffer* lineBuffer = m_lineCb.Get();
    m_context->VSSetConstantBuffers(0, 1, &lineBuffer);
    m_context->PSSetConstantBuffers(0, 1, &lineBuffer);

    LineConstants constants{};
    constants.viewProj = viewProj;
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    // (a) grid + plate: depth test on (model occludes), no write.
    if (ranges.gridCount > 0) {
        constants.params = {w, h, 0.0f, 1.0f};
        UpdateConstantBuffer(m_context.Get(), m_lineCb.Get(), constants);
        m_context->OMSetDepthStencilState(m_depthReadOnly.Get(), 0);
        m_context->Draw(ranges.gridCount, ranges.gridStart);
    }

    if (ranges.bboxCount > 0) {
        // (b) hidden part of the bounding box: depth off, faint.
        constants.params = {w, h, 0.0f, 0.22f};
        UpdateConstantBuffer(m_context.Get(), m_lineCb.Get(), constants);
        m_context->OMSetDepthStencilState(m_depthOff.Get(), 0);
        m_context->Draw(ranges.bboxCount, ranges.bboxStart);

        // (c) visible part: depth test + small offset, fully opaque.
        constants.params = {w, h, 4.0e-4f, 1.0f};
        UpdateConstantBuffer(m_context.Get(), m_lineCb.Get(), constants);
        m_context->OMSetDepthStencilState(m_depthReadOnly.Get(), 0);
        m_context->Draw(ranges.bboxCount, ranges.bboxStart);
    }

    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
}

void Renderer::DrawFeatureEdges(const DirectX::XMFLOAT4X4& viewProj, bool dark, float lineScale,
                                float depthOffsetClip, float alphaScale) {
    if (!m_edgeBuffer || m_edgeVertexCount == 0 || !m_edgeVs || !m_edgePs || !m_edgeLayout || !m_edgeCb) return;

    EdgeConstants constants{};
    constants.viewProj = viewProj;
    // The model is light grey in both themes, so edges are dark; slightly stronger in the dark theme.
    // In the 2× supersampled thumbnail a 1-pixel line fades when halved: alpha is raised.
    const float alpha =
        ((dark ? 0.80f : 0.72f) + (lineScale >= 1.75f ? 0.18f : 0.0f)) * std::clamp(alphaScale, 0.0f, 1.0f);
    constants.color = dark ? DirectX::XMFLOAT4{0.05f, 0.06f, 0.08f, std::min(alpha, 1.0f)}
                           : DirectX::XMFLOAT4{0.10f, 0.12f, 0.15f, std::min(alpha, 1.0f)};
    constants.params = {depthOffsetClip, 0.0f, 0.0f, 0.0f};
    UpdateConstantBuffer(m_context.Get(), m_edgeCb.Get(), constants);

    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_context->OMSetBlendState(m_blendAlpha.Get(), blendFactor, 0xFFFFFFFFu);
    m_context->OMSetDepthStencilState(m_depthReadOnly.Get(), 0);
    m_context->RSSetState(m_rasterizer.Get());
    m_context->IASetInputLayout(m_edgeLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    const UINT stride = 3 * sizeof(float);
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffer = m_edgeBuffer.Get();
    m_context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    m_context->VSSetShader(m_edgeVs.Get(), nullptr, 0);
    m_context->PSSetShader(m_edgePs.Get(), nullptr, 0);
    ID3D11Buffer* edgeBuffer = m_edgeCb.Get();
    m_context->VSSetConstantBuffers(0, 1, &edgeBuffer);
    m_context->PSSetConstantBuffers(0, 1, &edgeBuffer);
    m_context->Draw(m_edgeVertexCount, 0);

    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::ClearExtraStages() {
    // Direct2D or another user may have left state behind in between.
    m_context->GSSetShader(nullptr, nullptr, 0);
    m_context->HSSetShader(nullptr, nullptr, 0);
    m_context->DSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* nullSource[] = {nullptr};
    m_context->PSSetShaderResources(0, 1, nullSource);
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
}

bool Renderer::Render(UINT width, UINT height, const CameraState& camera, const RenderOptions& options, UINT samples,
                      float fitSmoothing) {
    using namespace DirectX;

    if (!m_context) return false;
    if (!EnsureTargets(width, height, samples)) return false;

    const bool hasMesh = m_vertexBuffer && m_vertexCount > 0;
    const bool dark = IsDarkBackground(camera.background);

    // --- shared state + background ---
    ClearExtraStages();

    BackgroundConstants backgroundConstants{};
    const XMFLOAT3& bg = camera.background;
    backgroundConstants.topColor = {
        bg.x + (1.0f - bg.x) * 0.18f,
        bg.y + (1.0f - bg.y) * 0.18f,
        bg.z + (1.0f - bg.z) * 0.18f,
        1.0f};
    backgroundConstants.bottomColor = {bg.x * 0.93f, bg.y * 0.93f, bg.z * 0.93f, 1.0f};

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &viewport);

    ID3D11RenderTargetView* renderTargets[] = {m_targets.colorRtv.Get()};
    m_context->OMSetRenderTargets(1, renderTargets, m_targets.depthDsv.Get());

    const float clearColor[4] = {bg.x, bg.y, bg.z, 1.0f};
    m_context->ClearRenderTargetView(m_targets.colorRtv.Get(), clearColor);
    m_context->ClearDepthStencilView(m_targets.depthDsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    m_context->RSSetState(m_rasterizer.Get());
    m_context->OMSetDepthStencilState(m_depthOff.Get(), 0);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_fullscreenVs.Get(), nullptr, 0);
    m_context->PSSetShader(m_backgroundPs.Get(), nullptr, 0);
    UpdateConstantBuffer(m_context.Get(), m_backgroundCb.Get(), backgroundConstants);
    ID3D11Buffer* backgroundBuffer = m_backgroundCb.Get();
    m_context->PSSetConstantBuffers(0, 1, &backgroundBuffer);
    m_context->Draw(3, 0);

    if (!hasMesh) {
        m_frame = FrameInfo{};
        if (m_samples > 1) {
            m_context->ResolveSubresource(m_targets.resolved.Get(), 0, m_targets.color.Get(), 0,
                                          DXGI_FORMAT_B8G8R8A8_UNORM);
        }
        return true;
    }

    // --- camera (Z up, right-handed) ---
    const float azimuth = camera.azimuthDeg;
    const float elevation = std::clamp(camera.elevationDeg, -90.0f, 90.0f);
    const XMFLOAT3 d = ViewDirection(azimuth, elevation);
    const XMFLOAT3 u = ViewUp(azimuth, elevation);
    const XMFLOAT3 target{0.0f, 0.0f, (m_bounds[2] + m_bounds[5]) * 0.5f};
    const GridLayout grid = ComputeGridLayout(m_bounds);

    float sceneRadius = m_radius;
    if (options.showGrid) {
        const float xs[2] = {grid.minX, grid.maxX};
        const float ys[2] = {grid.minY, grid.maxY};
        for (float x : xs)
            for (float y : ys)
                sceneRadius = std::max(sceneRadius, Distance3(x, y, 0.0f, target.x, target.y, target.z));
    }
    if (options.showPlate) {
        const float h = kPlateSizeMm * 0.5f;
        const float cs[2] = {-h, h};
        for (float x : cs)
            for (float y : cs)
                sceneRadius = std::max(sceneRadius, Distance3(x, y, 0.0f, target.x, target.y, target.z));
    }
    if (!std::isfinite(sceneRadius)) sceneRadius = 1.0f;
    sceneRadius = std::max(sceneRadius, 1e-3f) * 1.05f;
    const float eyeDistance = 2.2f * sceneRadius;

    const XMVECTOR targetV = XMLoadFloat3(&target);
    const XMVECTOR eye = XMVectorAdd(targetV, XMVectorScale(XMLoadFloat3(&d), eyeDistance));
    const XMMATRIX view = XMMatrixLookAtRH(eye, targetV, XMLoadFloat3(&u));
    const float zNear = eyeDistance - sceneRadius;
    const float zFar = eyeDistance + sceneRadius;

    const float aspect = static_cast<float>(width) / static_cast<float>(height);

    FitFrame fitTarget;
    if (options.stableFit) {
        // View-independent fit: a sphere around the pivot (target) enclosing the model (and the bounding box /
        // plate if on). The view always looks at target, so the centre is the middle of the screen;
        // the scale does not change while orbiting and the model stays in the window from every direction.
        float fitRadius = 0.0f;
        const auto grow = [&](float x, float y, float z) {
            fitRadius = std::max(fitRadius, Distance3(x, y, z, target.x, target.y, target.z));
        };
        if (m_fitPoints.empty() || options.showBBox) {
            for (int i = 0; i < 8; ++i)
                grow(m_bounds[(i & 1) ? 3 : 0], m_bounds[(i & 2) ? 4 : 1], m_bounds[(i & 4) ? 5 : 2]);
        } else {
            for (const XMFLOAT3& p : m_fitPoints) grow(p.x, p.y, p.z);
        }
        if (options.showPlate) {
            const float h = kPlateSizeMm * 0.5f;
            grow(-h, -h, 0.0f);
            grow(+h, -h, 0.0f);
            grow(+h, +h, 0.0f);
            grow(-h, +h, 0.0f);
        }
        if (!(fitRadius > 1e-6f) || !std::isfinite(fitRadius)) fitRadius = std::max(m_radius, 1e-3f);
        fitTarget.centerX = 0.0f;
        fitTarget.centerY = 0.0f;
        fitTarget.halfBase = fitRadius * std::max(1.0f, 1.0f / std::max(aspect, 0.01f)) * 1.04f;
    } else {
        fitTarget = ComputeFit(view, aspect, options.showPlate, options.showBBox);
    }
    if (!m_fitValid) {
        m_fit = fitTarget;
        m_fitValid = true;
    } else {
        const float smoothing = std::clamp(fitSmoothing, 0.0f, 1.0f);
        m_fit.centerX += (fitTarget.centerX - m_fit.centerX) * smoothing;
        m_fit.centerY += (fitTarget.centerY - m_fit.centerY) * smoothing;
        m_fit.halfBase += (fitTarget.halfBase - m_fit.halfBase) * smoothing;
    }

    const float zoom = std::clamp(std::isfinite(camera.zoom) ? camera.zoom : 1.0f, kMinZoom, kMaxZoom);
    const float halfHeight = std::max(m_fit.halfBase, 1e-4f) / zoom;
    const float halfWidth = halfHeight * aspect;
    const float panX = std::isfinite(camera.panX) ? camera.panX : 0.0f;
    const float panY = std::isfinite(camera.panY) ? camera.panY : 0.0f;
    const float cx = m_fit.centerX - panX;
    const float cy = m_fit.centerY - panY;

    const XMMATRIX projection =
        XMMatrixOrthographicOffCenterRH(cx - halfWidth, cx + halfWidth, cy - halfHeight, cy + halfHeight, zNear, zFar);
    const XMMATRIX viewProj = XMMatrixMultiply(view, projection);
    const float pixelsPerMm = static_cast<float>(height) / (2.0f * halfHeight);

    XMFLOAT4X4 viewProjStored{};
    XMStoreFloat4x4(&viewProjStored, viewProj);

    // --- model (solid) ---
    CameraConstants cameraConstants{};
    cameraConstants.viewProj = viewProjStored;
    XMStoreFloat4x4(&cameraConstants.view, view);
    cameraConstants.baseColor = kModelBaseColor;
    cameraConstants.params = {options.overhang ? 1.0f : 0.0f, kOverhangCos, kBedContactEpsMm, 0.0f};
    cameraConstants.params2 = {static_cast<float>(width), static_cast<float>(height), 0.0f, 0.0f};

    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    m_context->OMSetDepthStencilState(m_depthOn.Get(), 0);
    m_context->RSSetState(m_rasterizer.Get());
    m_context->IASetInputLayout(m_layout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_modelVs.Get(), nullptr, 0);
    m_context->PSSetShader(m_modelPs.Get(), nullptr, 0);

    UpdateConstantBuffer(m_context.Get(), m_cameraCb.Get(), cameraConstants);
    ID3D11Buffer* cameraBuffer = m_cameraCb.Get();
    m_context->VSSetConstantBuffers(0, 1, &cameraBuffer);
    m_context->PSSetConstantBuffers(0, 1, &cameraBuffer);

    const UINT stride = 6 * sizeof(float);
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffer = m_vertexBuffer.Get();
    m_context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    m_context->Draw(m_vertexCount, 0);

    // --- wireframe ---
    if (options.wireframe) {
        cameraConstants.baseColor = kWireColor;
        cameraConstants.params.w = 1.0f;
        UpdateConstantBuffer(m_context.Get(), m_cameraCb.Get(), cameraConstants);
        m_context->RSSetState(m_wireRasterizer.Get());
        m_context->OMSetDepthStencilState(m_depthLessEqual.Get(), 0);
        m_context->Draw(m_vertexCount, 0);
        m_context->RSSetState(m_rasterizer.Get());
    }

    // --- feature (sharp) edges ---
    if (options.showEdges && m_edgeVertexCount > 0) {
        // Offset of ~1.5 pixels toward the camera (mm -> clip depth): edges do not z-fight with faces even on slanted
        // surfaces, and hidden edges behind a thin wall (wall > ~0.5 mm) do not bleed through.
        const float offsetMm = 1.5f / std::max(pixelsPerMm, 1e-6f);
        const float depthOffset = std::min(offsetMm / std::max(zFar - zNear, 1e-6f), 0.01f);
        // Keep a densely edged model that is small on screen (far away / thumbnail / plate view) from turning
        // into a black blob: alpha drops as the ratio of edge pixels to model area grows.
        const double radiusPx = static_cast<double>(m_radius) * pixelsPerMm;
        const double areaPx = std::max(3.14159265 * radiusPx * radiusPx, 1.0);
        const double density = m_edgeLengthMm * pixelsPerMm * 0.5 / areaPx;
        // Scale: ANK-01 box body ~0.07 at 700 px, ~0.1 in a 256 px thumbnail (not faded); ~0.4 in a 64 px
        // thumbnail and ~0.76 for a 20 mm part in the K1 Max plate view (alpha drops to 0.3).
        const float fade = static_cast<float>(std::clamp(0.15 / std::max(density, 1e-9), 0.3, 1.0));
        DrawFeatureEdges(viewProjStored, dark, options.lineScale, depthOffset, fade);
    }

    // --- lines: grid, plate, bounding box ---
    FrameInfo frame;
    const float zGrid = -std::max(0.02f, sceneRadius * 1e-4f);
    try {
        const LineRanges ranges = BuildLines(options, dark, grid, zGrid, pixelsPerMm, frame);
        DrawLines(ranges, viewProjStored, width, height);
    } catch (...) {
        // Out of memory: lines are skipped in this frame, the model image stays.
        m_lineScratch.clear();
        frame.drawnStepMm = 0.0f;
        frame.fineGridDrawn = false;
    }

    if (m_samples > 1) {
        m_context->ResolveSubresource(m_targets.resolved.Get(), 0, m_targets.color.Get(), 0,
                                      DXGI_FORMAT_B8G8R8A8_UNORM);
    }

    frame.valid = true;
    frame.width = width;
    frame.height = height;
    frame.pixelsPerMm = pixelsPerMm;
    frame.grid = grid;
    frame.viewProj = viewProjStored;
    frame.viewDir = d;
    m_frame = frame;
    return true;
}

bool Renderer::ProjectToScreen(float sceneX, float sceneY, float sceneZ, float& screenX, float& screenY) const {
    if (!m_frame.valid) return false;
    const DirectX::XMMATRIX viewProj = DirectX::XMLoadFloat4x4(&m_frame.viewProj);
    DirectX::XMFLOAT3 p{};
    DirectX::XMStoreFloat3(
        &p, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(sceneX, sceneY, sceneZ, 1.0f), viewProj));
    screenX = (p.x * 0.5f + 0.5f) * static_cast<float>(m_frame.width);
    screenY = (1.0f - (p.y * 0.5f + 0.5f)) * static_cast<float>(m_frame.height);
    return true;
}

void Renderer::Blit(ID3D11RenderTargetView* target, UINT width, UINT height) {
    if (!m_context || !target || !m_targets.resolvedSrv) return;

    ClearExtraStages();

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &viewport);

    ID3D11RenderTargetView* renderTargets[] = {target};
    m_context->OMSetRenderTargets(1, renderTargets, nullptr);
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    m_context->OMSetDepthStencilState(m_depthOff.Get(), 0);
    m_context->RSSetState(m_rasterizer.Get());
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_fullscreenVs.Get(), nullptr, 0);
    m_context->PSSetShader(m_blitPs.Get(), nullptr, 0);

    ID3D11ShaderResourceView* sources[] = {m_targets.resolvedSrv.Get()};
    m_context->PSSetShaderResources(0, 1, sources);
    ID3D11SamplerState* samplers[] = {m_sampler.Get()};
    m_context->PSSetSamplers(0, 1, samplers);
    m_context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSource[] = {nullptr};
    m_context->PSSetShaderResources(0, 1, nullSource);
}

bool Renderer::ReadPixels(std::vector<uint8_t>& pixels, UINT& width, UINT& height) {
    if (!m_context || !m_targets.resolved) return false;

    if (!m_staging || m_stagingWidth != m_width || m_stagingHeight != m_height) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        m_staging.Reset();
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, m_staging.GetAddressOf()))) return false;
        m_stagingWidth = m_width;
        m_stagingHeight = m_height;
    }

    m_context->CopyResource(m_staging.Get(), m_targets.resolved.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

    const size_t rowBytes = static_cast<size_t>(m_width) * 4;
    try {
        pixels.resize(rowBytes * m_height);
    } catch (...) {
        m_context->Unmap(m_staging.Get(), 0);
        return false;
    }
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    for (UINT row = 0; row < m_height; ++row) {
        std::memcpy(pixels.data() + row * rowBytes, source + static_cast<size_t>(row) * mapped.RowPitch, rowBytes);
    }
    m_context->Unmap(m_staging.Get(), 0);

    width = m_width;
    height = m_height;
    return true;
}
