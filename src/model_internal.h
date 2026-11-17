#pragma once

// model_internal.h — internal interface of the "mesh" unit. Used only by stl_mesh.cpp, model_loader.cpp and
// model_3mf.cpp; other units work through model.h.
//
// Both parsers produce a raw triangle soup in ORIGINAL space (mm): 9 floats per triangle
// (ax ay az bx by bz cx cy cz). The shared finisher (BuildMeshFromTriangles) builds the Mesh.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "model.h"

// Load time limit (LoadModel's loadBudgetMs). 0 = unlimited. Long loops poll Expired() sparsely.
class LoadDeadline {
public:
    explicit LoadDeadline(uint32_t budgetMs = 0)
        : m_limited(budgetMs != 0),
          m_end(std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs)) {}
    bool Expired() const { return m_limited && std::chrono::steady_clock::now() >= m_end; }

private:
    bool m_limited;
    std::chrono::steady_clock::time_point m_end;
};

// Time exceeded: parsers/finisher throw, LoadModel / LoadStl catch it and report kLoadTimeoutMessage.
struct LoadTimeout {};
inline constexpr wchar_t kLoadTimeoutMessage[] = L"Model yüklemesi çok uzun sürdü";

inline void ThrowIfExpired(const LoadDeadline& deadline) {
    if (deadline.Expired()) throw LoadTimeout{};
}

// stl_mesh.cpp — binary or ASCII STL. lenient = true: the file is neither size-consistent binary nor ASCII;
// truncated binary was read as a last resort (may be random data; passed to BuildMeshFromTriangles as sizeGuard).
// Throws LoadTimeout when time runs out.
bool ParseStlTriangles(const uint8_t* data, size_t size, std::vector<float>& raw, std::wstring& error,
                       bool& lenient, const LoadDeadline& deadline);

// stl_mesh.cpp — time-limited LoadStl (used by LoadModel). Does not throw.
bool LoadStlWithDeadline(const void* data, size_t size, uint32_t maxTriangles, Mesh& out, std::wstring& error,
                         uint32_t flags, const LoadDeadline& deadline);

// model_3mf.cpp — 3MF package via OPC (msopc) + XmlLite. objectCount = number of merged build items.
// When time runs out: false and error = kLoadTimeoutMessage (does not throw).
bool Parse3mfTriangles(const uint8_t* data, size_t size, std::vector<float>& raw, uint32_t& objectCount,
                       std::wstring& error, const LoadDeadline& deadline);

// model_loader.cpp — invalid/degenerate triangle filtering, bounding box, volume, open/feature edges, decimation,
// translation to scene space. `raw` is consumed (empty on return). sizeGuard (suspicious input only, last-resort
// binary STL): fails with an invalid-size error if the bounding box exceeds 100 m on any axis.
// Time: throws LoadTimeout if exceeded during geometry steps; if exceeded during edge analysis the analysis
// is skipped (openEdges = -1, featureEdgeStatus = Skipped) and the model is still built.
bool BuildMeshFromTriangles(std::vector<float>& raw, ModelFormat format, uint32_t maxTriangles, uint32_t flags,
                            Mesh& out, std::wstring& error, const LoadDeadline& deadline, bool sizeGuard = false);
