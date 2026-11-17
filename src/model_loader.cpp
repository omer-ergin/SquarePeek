// model_loader.cpp — format detection, the shared Mesh finisher and number formatting helpers.

#include "model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>

#include "i18n.h"
#include "model_internal.h"
#include "stl_mesh.h"

namespace {

// ---------------------------------------------------------------------------------------------
// Format detection
// ---------------------------------------------------------------------------------------------

// Extension part of the hint: after the last '.'; if there is no '.', the whole hint (dotless extension).
const wchar_t* ExtensionOf(const wchar_t* hint) {
    if (!hint) return nullptr;
    const wchar_t* dot = std::wcsrchr(hint, L'.');
    return dot ? dot + 1 : hint;
}

ModelFormat FormatFromExtension(const wchar_t* hint) {
    const wchar_t* ext = ExtensionOf(hint);
    if (!ext || !*ext) return ModelFormat::Unknown;
    if (_wcsicmp(ext, L"stl") == 0) return ModelFormat::Stl;
    if (_wcsicmp(ext, L"3mf") == 0) return ModelFormat::ThreeMf;
    return ModelFormat::Unknown;
}

bool StartsWithZipLocalHeader(const void* data, size_t size) {
    static const uint8_t kMagic[4] = {'P', 'K', 0x03, 0x04};
    return data && size >= 4 && std::memcmp(data, kMagic, 4) == 0;
}

bool StartsWithPK(const void* data, size_t size) {
    return data && size >= 2 && std::memcmp(data, "PK", 2) == 0;
}

// ---------------------------------------------------------------------------------------------
// Vertex welding (for open edge analysis) — open-addressing hash table.
// Keys live in a dense array (vertex id = index); the table only stores id+1 (0 = empty).
// ---------------------------------------------------------------------------------------------

struct WeldKey {
    int64_t x, y, z;
};

inline uint64_t Mix64(uint64_t v) {
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return v;
}

// Seeded hash: vertex coordinates come from the file (attacker-controlled); the seed is random per welder, so
// colliding keys cannot be precomputed. Ids are assigned in insertion order, so the result does not depend on
// the seed.
inline uint64_t HashKey(const WeldKey& k, uint64_t seed) {
    uint64_t h = Mix64(static_cast<uint64_t>(k.x) ^ seed);
    h = Mix64(h ^ (static_cast<uint64_t>(k.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
    h = Mix64(h ^ (static_cast<uint64_t>(k.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)));
    return h;
}

// Probe budget exceeded (clustered hash: welding would go quadratic). Caught by AnalyzeEdges.
struct WeldBudgetExceeded {};

class VertexWelder {
public:
    explicit VertexWelder(size_t expectedVertices) {
        size_t capacity = 64;
        int bits = 6;
        while (capacity < expectedVertices * 2) {
            capacity <<= 1;
            ++bits;
        }
        m_table.assign(capacity, 0u);
        m_shift = 64 - bits;
        m_keys.reserve(expectedVertices);
        m_seed = Mix64(static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
                       static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this)));
    }

    uint32_t Insert(const WeldKey& key) {
        if ((m_keys.size() + 1) * 2 > m_table.size()) Grow();
        ++m_inserts;
        const size_t mask = m_table.size() - 1;
        size_t slot = Slot(key);
        for (;;) {
            const uint32_t entry = m_table[slot];
            if (entry == 0) {
                const uint32_t id = static_cast<uint32_t>(m_keys.size());
                m_keys.push_back(key);
                m_table[slot] = id + 1;
                return id;
            }
            const WeldKey& existing = m_keys[entry - 1];
            if (existing.x == key.x && existing.y == key.y && existing.z == key.z) return entry - 1;
            Probe();
            slot = (slot + 1) & mask;
        }
    }

    // Number of distinct vertices added so far.
    size_t Size() const { return m_keys.size(); }

private:
    // Index with the high bits (more robust against clustering in the low bits).
    size_t Slot(const WeldKey& key) const { return static_cast<size_t>(HashKey(key, m_seed) >> m_shift); }

    // Welding stops if total probes exceed an average of 64 per insert.
    void Probe() {
        if (++m_probes > 64 * m_inserts + m_table.size()) throw WeldBudgetExceeded{};
    }

    void Grow() {
        const size_t capacity = m_table.size() * 2;
        std::vector<uint32_t> table(capacity, 0u);
        const size_t mask = capacity - 1;
        --m_shift;
        for (size_t id = 0; id < m_keys.size(); ++id) {
            size_t slot = Slot(m_keys[id]);
            while (table[slot] != 0) {
                Probe();
                slot = (slot + 1) & mask;
            }
            table[slot] = static_cast<uint32_t>(id + 1);
        }
        m_table.swap(table);
    }

    std::vector<uint32_t> m_table;
    std::vector<WeldKey> m_keys;
    int m_shift = 58;
    uint64_t m_seed = 0;
    uint64_t m_inserts = 0;
    uint64_t m_probes = 0;
};

inline WeldKey MakeWeldKey(const float* p) {
    return WeldKey{std::llround(static_cast<double>(p[0]) / kWeldQuantumMm),
                   std::llround(static_cast<double>(p[1]) / kWeldQuantumMm),
                   std::llround(static_cast<double>(p[2]) / kWeldQuantumMm)};
}

// Do the face normals of two triangles diverge by more than kFeatureEdgeCos (30°)?
bool IsCrease(const float* a, const float* b) {
    const auto normal = [](const float* tri, double n[3]) {
        const double ux = static_cast<double>(tri[3]) - tri[0];
        const double uy = static_cast<double>(tri[4]) - tri[1];
        const double uz = static_cast<double>(tri[5]) - tri[2];
        const double vx = static_cast<double>(tri[6]) - tri[0];
        const double vy = static_cast<double>(tri[7]) - tri[1];
        const double vz = static_cast<double>(tri[8]) - tri[2];
        n[0] = uy * vz - uz * vy;
        n[1] = uz * vx - ux * vz;
        n[2] = ux * vy - uy * vx;
    };
    double na[3];
    double nb[3];
    normal(a, na);
    normal(b, nb);
    const double dot = na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2];
    const double la = na[0] * na[0] + na[1] * na[1] + na[2] * na[2];
    const double lb = nb[0] * nb[0] + nb[1] * nb[1] + nb[2] * nb[2];
    return dot < kFeatureEdgeCos * std::sqrt(la * lb);
}

struct EdgeAnalysis {
    int64_t openEdges = -1;   // undirected edges whose use count is not 2; -1 = out of memory
    FeatureEdgeStatus status = FeatureEdgeStatus::Skipped;
};

// Edge analysis: open edge count and (if wantFeatures) feature edges (scene space, model.h).
// Vertices are welded and a vertex -> triangle adjacency (CSR) is built; each undirected edge is processed once,
// in the lowest-index triangle that uses it. If its use count is not 2 it is an open edge (boundary /
// non-manifold) and is drawn; if 2, the two face normals are compared. If time runs out (deadline) the
// analysis is skipped.
EdgeAnalysis AnalyzeEdges(const std::vector<float>& tris, size_t triangleCount, const double offset[3],
                          bool wantFeatures, double minChainMm, std::vector<float>& featureEdges,
                          const LoadDeadline& deadline) {
    constexpr uint32_t kSkip = 0xFFFFFFFFu;
    EdgeAnalysis result;
    featureEdges.clear();
    // Work counter: the time is polled every ~64K steps.
    uint64_t work = 0;
    const auto tick = [&](uint64_t amount) {
        const uint64_t before = work;
        work += amount;
        if ((before >> 16) != (work >> 16)) ThrowIfExpired(deadline);
    };
    try {
        // 1. 3 welded vertex ids per triangle; after welding, the first id of a degenerate one is kSkip.
        std::vector<uint32_t> ids(triangleCount * 3);
        size_t vertexCount = 0;
        {
            VertexWelder welder(triangleCount / 2 + 16);
            for (size_t t = 0; t < triangleCount; ++t) {
                tick(1);
                const float* tri = &tris[t * 9];
                const uint32_t i0 = welder.Insert(MakeWeldKey(tri));
                const uint32_t i1 = welder.Insert(MakeWeldKey(tri + 3));
                const uint32_t i2 = welder.Insert(MakeWeldKey(tri + 6));
                ids[t * 3 + 0] = (i0 == i1 || i1 == i2 || i0 == i2) ? kSkip : i0;
                ids[t * 3 + 1] = i1;
                ids[t * 3 + 2] = i2;
            }
            vertexCount = welder.Size();
        }

        // 2. Vertex -> triangle list (CSR). start[v] .. start[v + 1].
        std::vector<uint32_t> start(vertexCount + 1, 0u);
        for (size_t t = 0; t < triangleCount; ++t) {
            if (ids[t * 3] == kSkip) continue;
            for (int k = 0; k < 3; ++k) ++start[ids[t * 3 + k] + 1];
        }
        for (size_t v = 0; v < vertexCount; ++v) start[v + 1] += start[v];
        std::vector<uint32_t> adjacency(start[vertexCount]);
        for (size_t t = 0; t < triangleCount; ++t) {
            if (ids[t * 3] == kSkip) continue;
            for (int k = 0; k < 3; ++k) adjacency[start[ids[t * 3 + k]]++] = static_cast<uint32_t>(t);
        }
        for (size_t v = vertexCount; v > 0; --v) start[v] = start[v - 1];
        start[0] = 0;

        // 3. Edges. Each undirected edge {v, w} (v < w) is processed once at its smaller vertex v: the
        //    (w << 32 | triangle) pairs of v's triangles are collected and sorted. A group with the same w = the
        //    triangles using the edge (ascending index): group size = use count, first = owner (lowest-index
        //    triangle), second = opposite face.
        //    Total work is O(n log n); it does not grow quadratically even when many triangles share a vertex
        //    (fan / "book").
        // Candidate edge = (triangle << 2) | edge number; kBoundaryBit: open edge (boundary / non-manifold).
        constexpr uint64_t kBoundaryBit = 1ull << 40;
        const size_t maxCandidates = kFeatureEdgeMaxSegments * 4;   // memory limit (TooMany if exceeded)
        std::vector<uint64_t> candidates;
        bool tooMany = false;
        const auto addCandidate = [&](size_t t, int e, bool boundary) {
            if (!wantFeatures || tooMany) return;
            if (candidates.size() >= maxCandidates) {
                tooMany = true;
                return;
            }
            candidates.push_back((static_cast<uint64_t>(t) << 2) | static_cast<uint64_t>(e) |
                                 (boundary ? kBoundaryBit : 0));
        };
        // Number e of edge {v, w} in the owner triangle: (ids[e], ids[(e + 1) % 3]) = {v, w}.
        const auto edgeIndex = [&ids](uint32_t triangle, uint32_t v, uint32_t w) {
            const uint32_t* q = &ids[static_cast<size_t>(triangle) * 3];
            for (int e = 0; e < 2; ++e) {
                const uint32_t p0 = q[e];
                const uint32_t p1 = q[e + 1];
                if ((p0 == v && p1 == w) || (p0 == w && p1 == v)) return e;
            }
            return 2;
        };

        int64_t open = 0;
        std::vector<uint64_t> local;
        for (size_t v = 0; v < vertexCount; ++v) {
            const uint32_t self = static_cast<uint32_t>(v);
            local.clear();
            for (uint32_t i = start[v]; i < start[v + 1]; ++i) {
                const uint32_t u = adjacency[i];
                const uint32_t* q = &ids[static_cast<size_t>(u) * 3];
                for (int k = 0; k < 3; ++k) {
                    if (q[k] > self) local.push_back((static_cast<uint64_t>(q[k]) << 32) | u);
                }
            }
            if (local.empty()) continue;
            tick(local.size() + 1);
            if (local.size() > 1) std::sort(local.begin(), local.end());
            for (size_t g = 0; g < local.size();) {
                const uint32_t w = static_cast<uint32_t>(local[g] >> 32);
                size_t h = g + 1;
                while (h < local.size() && static_cast<uint32_t>(local[h] >> 32) == w) ++h;
                const size_t users = h - g;
                const uint32_t owner = static_cast<uint32_t>(local[g]);
                if (users != 2) {
                    ++open;
                    addCandidate(owner, edgeIndex(owner, self, w), true);
                } else if (wantFeatures && !tooMany) {
                    const uint32_t other = static_cast<uint32_t>(local[g + 1]);
                    if (IsCrease(&tris[static_cast<size_t>(owner) * 9], &tris[static_cast<size_t>(other) * 9])) {
                        addCandidate(owner, edgeIndex(owner, self, w), false);
                    }
                }
                g = h;
            }
        }
        std::vector<uint64_t>().swap(local);
        result.openEdges = open;
        if (!wantFeatures) return result;

        std::vector<uint32_t>().swap(adjacency);
        std::vector<uint32_t>().swap(start);
        if (tooMany) {
            result.status = FeatureEdgeStatus::TooMany;
            return result;
        }
        // Candidates were collected in vertex order; keep the output order (and the order in which chain
        // lengths are summed) in triangle/edge order, as before.
        tick(candidates.size());
        std::sort(candidates.begin(), candidates.end(),
                  [](uint64_t x, uint64_t y) { return (x & ~kBoundaryBit) < (y & ~kBoundaryBit); });

        // 4. Noise filter: sharp edges are joined at their vertices into chains (union-find); chains whose
        //    total length is under minChainMm are dropped. On scanned / organic meshes, the odd wrinkled
        //    triangles produce short, isolated "hair" lines; CAD edges are long chains/loops.
        //    Open edges (boundary / non-manifold) are always kept.
        const auto segment = [&](uint64_t c, const float*& p, const float*& q, uint32_t& a, uint32_t& b) {
            const size_t t = static_cast<size_t>((c & ~kBoundaryBit) >> 2);
            const int e = static_cast<int>(c & 3);
            const int f = (e + 1) % 3;
            p = &tris[t * 9 + static_cast<size_t>(e) * 3];
            q = &tris[t * 9 + static_cast<size_t>(f) * 3];
            a = ids[t * 3 + e];
            b = ids[t * 3 + f];
        };
        std::vector<uint32_t> parent(vertexCount);
        for (size_t v = 0; v < vertexCount; ++v) parent[v] = static_cast<uint32_t>(v);
        const auto find = [&parent](uint32_t v) {
            while (parent[v] != v) {
                parent[v] = parent[parent[v]];
                v = parent[v];
            }
            return v;
        };
        tick(candidates.size() * 3);
        for (const uint64_t c : candidates) {
            if ((c & kBoundaryBit) != 0) continue;
            const float* p;
            const float* q;
            uint32_t a, b;
            segment(c, p, q, a, b);
            const uint32_t ra = find(a);
            const uint32_t rb = find(b);
            if (ra != rb) parent[ra] = rb;
        }
        std::vector<float> chainLength(vertexCount, 0.0f);
        for (const uint64_t c : candidates) {
            if ((c & kBoundaryBit) != 0) continue;
            const float* p;
            const float* q;
            uint32_t a, b;
            segment(c, p, q, a, b);
            const double dx = static_cast<double>(q[0]) - p[0];
            const double dy = static_cast<double>(q[1]) - p[1];
            const double dz = static_cast<double>(q[2]) - p[2];
            chainLength[find(a)] += static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        const auto keep = [&](uint64_t c) {
            if ((c & kBoundaryBit) != 0) return true;
            const float* p;
            const float* q;
            uint32_t a, b;
            segment(c, p, q, a, b);
            return static_cast<double>(chainLength[find(a)]) >= minChainMm;
        };

        // 5. Write to scene space.
        size_t kept = 0;
        {
            for (const uint64_t c : candidates) kept += keep(c) ? 1 : 0;
            tooMany = kept > kFeatureEdgeMaxSegments;
        }
        if (tooMany) {
            result.status = FeatureEdgeStatus::TooMany;
            return result;
        }
        featureEdges.reserve(kept * 6);
        for (const uint64_t c : candidates) {
            if (!keep(c)) continue;
            const float* p;
            const float* q;
            uint32_t a, b;
            segment(c, p, q, a, b);
            for (int k = 0; k < 3; ++k) featureEdges.push_back(static_cast<float>(static_cast<double>(p[k]) - offset[k]));
            for (int k = 0; k < 3; ++k) featureEdges.push_back(static_cast<float>(static_cast<double>(q[k]) - offset[k]));
        }
        result.status = FeatureEdgeStatus::Ok;
        return result;
    } catch (const std::bad_alloc&) {
        std::vector<float>().swap(featureEdges);
        return EdgeAnalysis{};
    } catch (const WeldBudgetExceeded&) {
        // Malicious / pathological vertex distribution: open edges "unknown", no edge lines.
        std::vector<float>().swap(featureEdges);
        return EdgeAnalysis{};
    } catch (const LoadTimeout&) {
        // Out of time: geometry is ready, edges "unknown" (the model is still shown).
        std::vector<float>().swap(featureEdges);
        return EdgeAnalysis{};
    }
}

// Typical print shell volume (mm³), from local thickness (model.h, kTypicalWallMm). < 0 if not computable.
// inwardSign: +1 if the signed volume is >= 0 (winding faces outward, inward = -normal), otherwise -1.
// Samples are area-weighted and chosen systematically (deterministic); since rays are short (at most
// 2 × shell), only the grid cells (≤ 8) touched by the ray segment's box are tested.
double EstimateShellVolume(const std::vector<float>& tris, size_t count, const double mn[3], const double mx[3],
                           double totalArea, double volume, double inwardSign, const LoadDeadline& deadline) {
    constexpr size_t kMaxSamples = 20'000;
    constexpr size_t kMaxCells = 4u << 20;
    constexpr size_t kMaxRefs = 24u << 20;
    constexpr uint64_t kMaxRayTests = 200'000'000;   // work limit (badly distributed meshes): fallback if exceeded
    const double maxDepth = std::max(kTypicalWallMm, kTypicalTopBottomMm);
    const double rayLength = 2.0 * maxDepth;
    if (count == 0 || !(totalArea > 0) || !(volume > 0)) return -1.0;
    try {
        // 1. Uniform grid: cell ≥ ray length; the cell is enlarged if there are too many cells or references.
        double extent[3];
        for (int k = 0; k < 3; ++k) extent[k] = std::max(mx[k] - mn[k], 1e-6);
        double cell = std::max(rayLength * 1.05, std::cbrt(extent[0] * extent[1] * extent[2] / (2u << 20)));
        size_t dims[3]{};
        std::vector<uint32_t> start;
        std::vector<uint32_t> refs;
        const auto cellRange = [&](const float* tri, size_t lo[3], size_t hi[3]) {
            for (int k = 0; k < 3; ++k) {
                const double a = std::min({tri[k], tri[3 + k], tri[6 + k]}) - mn[k];
                const double b = std::max({tri[k], tri[3 + k], tri[6 + k]}) - mn[k];
                lo[k] = static_cast<size_t>(std::clamp(std::floor(a / cell), 0.0, static_cast<double>(dims[k] - 1)));
                hi[k] = static_cast<size_t>(std::clamp(std::floor(b / cell), 0.0, static_cast<double>(dims[k] - 1)));
            }
        };
        for (int attempt = 0;; ++attempt) {
            if (attempt >= 6) return -1.0;
            for (int k = 0; k < 3; ++k) dims[k] = static_cast<size_t>(std::ceil(extent[k] / cell)) + 1;
            const double cells = static_cast<double>(dims[0]) * static_cast<double>(dims[1]) * static_cast<double>(dims[2]);
            if (cells > kMaxCells) {
                cell *= 2.0;
                continue;
            }
            start.assign(static_cast<size_t>(cells) + 1, 0u);
            uint64_t total = 0;
            for (size_t t = 0; t < count && total <= kMaxRefs; ++t) {
                if ((t & 0xFFFF) == 0xFFFF) ThrowIfExpired(deadline);
                size_t lo[3], hi[3];
                cellRange(&tris[t * 9], lo, hi);
                total += static_cast<uint64_t>(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1);
            }
            if (total > kMaxRefs) {
                cell *= 2.0;
                continue;
            }
            refs.resize(static_cast<size_t>(total));
            break;
        }
        const size_t cellCount = start.size() - 1;
        const auto cellIndex = [&](size_t x, size_t y, size_t z) { return (z * dims[1] + y) * dims[0] + x; };
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t t = 0; t < count; ++t) {
                if ((t & 0xFFFF) == 0xFFFF) ThrowIfExpired(deadline);
                size_t lo[3], hi[3];
                cellRange(&tris[t * 9], lo, hi);
                for (size_t z = lo[2]; z <= hi[2]; ++z)
                    for (size_t y = lo[1]; y <= hi[1]; ++y)
                        for (size_t x = lo[0]; x <= hi[0]; ++x) {
                            const size_t c = cellIndex(x, y, z);
                            if (pass == 0) ++start[c + 1];
                            else refs[start[c]++] = static_cast<uint32_t>(t);
                        }
            }
            if (pass == 0) {
                for (size_t c = 0; c < cellCount; ++c) start[c + 1] += start[c];
            } else {
                for (size_t c = cellCount; c > 0; --c) start[c] = start[c - 1];
                start[0] = 0;
            }
        }

        // 2. Samples: evenly spaced along the area axis (interval midpoints), position within the triangle via R2.
        const size_t samples = kMaxSamples;
        const double stepArea = totalArea / static_cast<double>(samples);
        double shell = 0.0;
        double cumulative = 0.0;
        double nextSample = stepArea * 0.5;
        size_t taken = 0;
        uint64_t rayTests = 0;
        for (size_t t = 0; t < count && taken < samples; ++t) {
            const float* tri = &tris[t * 9];
            const double ux = static_cast<double>(tri[3]) - tri[0], uy = static_cast<double>(tri[4]) - tri[1],
                         uz = static_cast<double>(tri[5]) - tri[2];
            const double vx = static_cast<double>(tri[6]) - tri[0], vy = static_cast<double>(tri[7]) - tri[1],
                         vz = static_cast<double>(tri[8]) - tri[2];
            double n[3] = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
            const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            cumulative += 0.5 * len;
            if (cumulative < nextSample) continue;
            for (int k = 0; k < 3; ++k) n[k] /= len;
            const double faceDepth = std::fabs(n[2]) > 0.7 ? kTypicalTopBottomMm : kTypicalWallMm;
            const double dir[3] = {-n[0] * inwardSign, -n[1] * inwardSign, -n[2] * inwardSign};
            while (cumulative >= nextSample && taken < samples) {
                ThrowIfExpired(deadline);
                nextSample += stepArea;
                ++taken;
                double a = std::fmod(0.5 + static_cast<double>(taken) * 0.7548776662466927, 1.0);
                double b = std::fmod(0.5 + static_cast<double>(taken) * 0.5698402909980532, 1.0);
                if (a + b > 1.0) {
                    a = 1.0 - a;
                    b = 1.0 - b;
                }
                const double origin[3] = {tri[0] + a * ux + b * vx, tri[1] + a * uy + b * vy, tri[2] + a * uz + b * vz};
                // Nearest opposite face: cells of the box of ray segment [origin, origin + dir × rayLength].
                double nearest = rayLength;
                size_t lo[3], hi[3];
                for (int k = 0; k < 3; ++k) {
                    const double p = origin[k] - mn[k];
                    const double q = p + dir[k] * rayLength;
                    lo[k] = static_cast<size_t>(std::clamp(std::floor(std::min(p, q) / cell), 0.0, static_cast<double>(dims[k] - 1)));
                    hi[k] = static_cast<size_t>(std::clamp(std::floor(std::max(p, q) / cell), 0.0, static_cast<double>(dims[k] - 1)));
                }
                for (size_t z = lo[2]; z <= hi[2]; ++z)
                    for (size_t y = lo[1]; y <= hi[1]; ++y)
                        for (size_t x = lo[0]; x <= hi[0]; ++x) {
                            const size_t c = cellIndex(x, y, z);
                            rayTests += start[c + 1] - start[c];
                            if (rayTests > kMaxRayTests) return -1.0;
                            for (uint32_t r = start[c]; r < start[c + 1]; ++r) {
                                const uint32_t o = refs[r];
                                if (o == t) continue;
                                // Möller–Trumbore (double-sided).
                                const float* w = &tris[static_cast<size_t>(o) * 9];
                                const double e1[3] = {static_cast<double>(w[3]) - w[0], static_cast<double>(w[4]) - w[1],
                                                      static_cast<double>(w[5]) - w[2]};
                                const double e2[3] = {static_cast<double>(w[6]) - w[0], static_cast<double>(w[7]) - w[1],
                                                      static_cast<double>(w[8]) - w[2]};
                                const double pv[3] = {dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2],
                                                      dir[0] * e2[1] - dir[1] * e2[0]};
                                const double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
                                if (std::fabs(det) < 1e-18) continue;
                                const double inv = 1.0 / det;
                                const double tv[3] = {origin[0] - w[0], origin[1] - w[1], origin[2] - w[2]};
                                const double bu = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
                                if (bu < 0.0 || bu > 1.0) continue;
                                const double qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2],
                                                      tv[0] * e1[1] - tv[1] * e1[0]};
                                const double bv = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) * inv;
                                if (bv < 0.0 || bu + bv > 1.0) continue;
                                const double hit = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
                                if (hit > 1e-5 && hit < nearest) nearest = hit;
                            }
                        }
                shell += stepArea * std::min(faceDepth, nearest * 0.5);
            }
        }
        if (taken == 0) return -1.0;
        // Scale up if the last few samples were missed due to floating point.
        shell *= static_cast<double>(samples) / static_cast<double>(taken);
        return std::min(shell, volume);
    } catch (const std::bad_alloc&) {
        return -1.0;
    } catch (const LoadTimeout&) {
        return -1.0;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Shared finisher
// ---------------------------------------------------------------------------------------------

bool BuildMeshFromTriangles(std::vector<float>& raw, ModelFormat format, uint32_t maxTriangles, uint32_t flags,
                            Mesh& out, std::wstring& error, const LoadDeadline& deadline, bool sizeGuard) {
    ThrowIfExpired(deadline);
    out = Mesh{};
    size_t count = raw.size() / 9;

    // 1. Validity: finite and |coordinate| <= 1e9. Compact in place.
    size_t kept = 0;
    for (size_t t = 0; t < count; ++t) {
        if ((t & 0xFFFFF) == 0xFFFFF) ThrowIfExpired(deadline);
        const float* tri = &raw[t * 9];
        bool valid = true;
        for (int k = 0; k < 9; ++k) {
            const float c = tri[k];
            if (!std::isfinite(c) || std::fabs(static_cast<double>(c)) > 1e9) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        if (kept != t) std::memmove(&raw[kept * 9], tri, 9 * sizeof(float));
        ++kept;
    }
    count = kept;
    raw.resize(count * 9);
    if (count == 0) {
        std::vector<float>().swap(raw);
        error = L"Dosyada kullanılabilir üçgen yok";
        return false;
    }

    // 2. Bounding box in original space (degenerate triangles included).
    double mn[3] = {raw[0], raw[1], raw[2]};
    double mx[3] = {raw[0], raw[1], raw[2]};
    for (size_t i = 0; i < count * 3; ++i) {
        const float* p = &raw[i * 3];
        for (int k = 0; k < 3; ++k) {
            const double c = p[k];
            if (c < mn[k]) mn[k] = c;
            if (c > mx[k]) mx[k] = c;
        }
    }
    // Garbage data (random bytes) yields a "part" kilometres across. The 100 m limit applies only to suspicious
    // input: in well-formed ASCII / size-consistent binary / 3MF files, huge or micron-unit models are valid.
    constexpr double kMaxModelSizeMm = 100000.0;  // 100 m
    for (int k = 0; k < 3; ++k) {
        const double extent = mx[k] - mn[k];
        if (!std::isfinite(mn[k]) || !std::isfinite(mx[k]) || !std::isfinite(extent) ||
            (sizeGuard && extent > kMaxModelSizeMm)) {
            std::vector<float>().swap(raw);
            error = L"Model boyutu geçersiz (100 m üstü)";
            return false;
        }
    }
    double center[3];
    for (int k = 0; k < 3; ++k) {
        out.origMin[k] = mn[k];
        out.origMax[k] = mx[k];
        out.size[k] = mx[k] - mn[k];
        center[k] = (mn[k] + mx[k]) * 0.5;
    }
    out.sceneOffset[0] = center[0];
    out.sceneOffset[1] = center[1];
    out.sceneOffset[2] = mn[2];

    // 3. Drop degenerate triangles (they already contributed to the bounding box).
    ThrowIfExpired(deadline);
    kept = 0;
    for (size_t t = 0; t < count; ++t) {
        const float* tri = &raw[t * 9];
        const double ux = static_cast<double>(tri[3]) - tri[0];
        const double uy = static_cast<double>(tri[4]) - tri[1];
        const double uz = static_cast<double>(tri[5]) - tri[2];
        const double vx = static_cast<double>(tri[6]) - tri[0];
        const double vy = static_cast<double>(tri[7]) - tri[1];
        const double vz = static_cast<double>(tri[8]) - tri[2];
        const double nx = uy * vz - uz * vy;
        const double ny = uz * vx - ux * vz;
        const double nz = ux * vy - uy * vx;
        if (std::sqrt(nx * nx + ny * ny + nz * nz) < 1e-12) continue;
        if (kept != t) std::memmove(&raw[kept * 9], tri, 9 * sizeof(float));
        ++kept;
    }
    count = kept;
    raw.resize(count * 9);
    if (count == 0) {
        std::vector<float>().swap(raw);
        out = Mesh{};
        error = L"Dosyada kullanılabilir üçgen yok";
        return false;
    }
    out.sourceTriangleCount = count;

    // 4. Volume (signed tetrahedra, relative to the box centre for numerical stability) and surface area.
    double volume = 0.0;
    double area2 = 0.0;   // twice the sum of triangle areas
    for (size_t t = 0; t < count; ++t) {
        const float* tri = &raw[t * 9];
        const double ax = tri[0] - center[0], ay = tri[1] - center[1], az = tri[2] - center[2];
        const double bx = tri[3] - center[0], by = tri[4] - center[1], bz = tri[5] - center[2];
        const double cx = tri[6] - center[0], cy = tri[7] - center[1], cz = tri[8] - center[2];
        volume += ax * (by * cz - bz * cy) + ay * (bz * cx - bx * cz) + az * (bx * cy - by * cx);
        const double ux = bx - ax, uy = by - ay, uz = bz - az;
        const double vx = cx - ax, vy = cy - ay, vz = cz - az;
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        area2 += std::sqrt(nx * nx + ny * ny + nz * nz);
    }
    ThrowIfExpired(deadline);
    out.volumeMm3 = std::fabs(volume / 6.0);
    out.surfaceAreaMm2 = area2 / 2.0;

    // 5. Open edges and feature edges (over the full triangle set, before decimation).
    const bool wantOpenEdges = (flags & kModelLoadSkipTopology) == 0;
    const bool wantFeatureEdges = (flags & kModelLoadSkipFeatureEdges) == 0;
    out.openEdges = -1;
    out.featureEdgeStatus = FeatureEdgeStatus::Skipped;
    if ((wantOpenEdges || wantFeatureEdges) && count <= kTopologyMaxTriangles) {
        // Noise filter threshold: 1% of the bounding box diagonal (~1.9 mm for a 135 mm box).
        const double diagonal = std::sqrt(out.size[0] * out.size[0] + out.size[1] * out.size[1] +
                                          out.size[2] * out.size[2]);
        const EdgeAnalysis analysis =
            AnalyzeEdges(raw, count, out.sceneOffset, wantFeatureEdges, diagonal * 0.01, out.featureEdges, deadline);
        if (wantOpenEdges) out.openEdges = analysis.openEdges;
        out.featureEdgeStatus = analysis.status;
    }

    // 5b. Typical print shell volume (local thickness). Not needed for thumbnails (kModelLoadSkipTopology); if
    //     time runs out or the mesh is too large it stays -1 and the mass uses the fallback formula.
    out.printShellMm3 = -1.0;
    if (wantOpenEdges && count <= kTopologyMaxTriangles) {
        out.printShellMm3 = EstimateShellVolume(raw, count, mn, mx, out.surfaceAreaMm2, out.volumeMm3,
                                                volume >= 0 ? 1.0 : -1.0, deadline);
    }

    // 6. Decimation. (Bounded linearly and by the triangle limits; time is not polled here: even if edge
    //    analysis used up the time, the ready geometry is shown.)
    size_t step = 1;
    if (maxTriangles > 0 && count > maxTriangles) step = (count + maxTriangles - 1) / maxTriangles;
    out.decimated = step > 1;
    const size_t drawn = (count + step - 1) / step;

    // 7. Vertices: scene = original - sceneOffset; face normal.
    out.vertices.reserve(drawn * 18);
    const double* off = out.sceneOffset;
    for (size_t t = 0; t < count; t += step) {
        const float* tri = &raw[t * 9];
        const double ux = static_cast<double>(tri[3]) - tri[0];
        const double uy = static_cast<double>(tri[4]) - tri[1];
        const double uz = static_cast<double>(tri[5]) - tri[2];
        const double vx = static_cast<double>(tri[6]) - tri[0];
        const double vy = static_cast<double>(tri[7]) - tri[1];
        const double vz = static_cast<double>(tri[8]) - tri[2];
        double nx = uy * vz - uz * vy;
        double ny = uz * vx - ux * vz;
        double nz = ux * vy - uy * vx;
        const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        nx /= length;
        ny /= length;
        nz /= length;
        const float n[3] = {static_cast<float>(nx), static_cast<float>(ny), static_cast<float>(nz)};
        for (int v = 0; v < 3; ++v) {
            for (int k = 0; k < 3; ++k) {
                out.vertices.push_back(static_cast<float>(static_cast<double>(tri[v * 3 + k]) - off[k]));
            }
            out.vertices.push_back(n[0]);
            out.vertices.push_back(n[1]);
            out.vertices.push_back(n[2]);
        }
    }
    out.triangleCount = static_cast<uint32_t>(out.vertices.size() / 18);

    // 8. Scene bounds and radius.
    const double sx = out.size[0], sy = out.size[1], sz = out.size[2];
    out.bounds[0] = static_cast<float>(-sx * 0.5);
    out.bounds[1] = static_cast<float>(-sy * 0.5);
    out.bounds[2] = 0.0f;
    out.bounds[3] = static_cast<float>(sx * 0.5);
    out.bounds[4] = static_cast<float>(sy * 0.5);
    out.bounds[5] = static_cast<float>(sz);
    const double radius = 0.5 * std::sqrt(sx * sx + sy * sy + sz * sz);
    out.radius = radius < 1e-6 ? 1.0f : static_cast<float>(radius);
    out.format = format;
    out.objectCount = 1;

    // 9. Release the raw data (to lower peak memory use).
    std::vector<float>().swap(raw);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

ModelFormat DetectModelFormat(const void* data, size_t size, const wchar_t* fileNameOrExtHint) {
    const ModelFormat byHint = FormatFromExtension(fileNameOrExtHint);
    if (!data) return byHint;
    if (byHint == ModelFormat::Stl) {
        return StartsWithZipLocalHeader(data, size) ? ModelFormat::ThreeMf : ModelFormat::Stl;
    }
    // A file named .3mf is always tried as 3MF; if it is not a zip, LoadModel returns an error (so that random
    // bytes are not taken for an STL and shown as a meaningless model kilometres across).
    if (byHint == ModelFormat::ThreeMf) return ModelFormat::ThreeMf;
    return StartsWithZipLocalHeader(data, size) ? ModelFormat::ThreeMf : ModelFormat::Stl;
}

bool IsSupportedModelExtension(const wchar_t* fileNameOrExt) {
    return FormatFromExtension(fileNameOrExt) != ModelFormat::Unknown;
}

bool LoadModel(const void* data, size_t size, const wchar_t* fileNameOrExtHint, uint32_t maxTriangles, Mesh& out,
               std::wstring& error, uint32_t flags, uint32_t loadBudgetMs) {
    try {
        out = Mesh{};
        error.clear();
        const LoadDeadline deadline(loadBudgetMs);

        switch (DetectModelFormat(data, size, fileNameOrExtHint)) {
        case ModelFormat::Stl:
            return LoadStlWithDeadline(data, size, maxTriangles, out, error, flags, deadline);
        case ModelFormat::ThreeMf: {
            if (!StartsWithPK(data, size)) {
                error = L"3MF paketi açılamadı (zip değil)";
                return false;
            }
            std::vector<float> raw;
            uint32_t objectCount = 0;
            if (!Parse3mfTriangles(static_cast<const uint8_t*>(data), size, raw, objectCount, error, deadline)) {
                out = Mesh{};
                return false;
            }
            if (!BuildMeshFromTriangles(raw, ModelFormat::ThreeMf, maxTriangles, flags, out, error, deadline)) {
                out = Mesh{};
                return false;
            }
            out.objectCount = std::max<uint32_t>(objectCount, 1);
            return true;
        }
        default:
            error = L"Desteklenmeyen dosya türü";
            return false;
        }
    } catch (const LoadTimeout&) {
        out = Mesh{};
        error = kLoadTimeoutMessage;
        return false;
    } catch (const std::bad_alloc&) {
        out = Mesh{};
        error = L"Bellek yetersiz";
        return false;
    } catch (...) {
        out = Mesh{};
        error = L"Dosya çözümlenemedi";
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Number formatting (locale-independent; separators follow the UI language: i18n.h)
// ---------------------------------------------------------------------------------------------

std::wstring FormatDecimalTr(double value, int decimals) {
    if (!std::isfinite(value)) return L"?";
    decimals = std::clamp(decimals, 0, 6);
    uint64_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    const double scaled = std::fabs(value) * static_cast<double>(scale);
    if (scaled >= 9.0e18) return L"?";
    const uint64_t rounded = static_cast<uint64_t>(std::llround(scaled));

    const uint64_t integerPart = rounded / scale;
    const uint64_t fraction = rounded % scale;

    std::wstring text;
    if (value < 0 && rounded != 0) text.push_back(L'-');
    text += std::to_wstring(integerPart);
    if (decimals > 0) {
        std::wstring frac = std::to_wstring(fraction);
        text.push_back(UiTurkish() ? L',' : L'.');
        text.append(static_cast<size_t>(decimals) - frac.size(), L'0');
        text += frac;
    }
    return text;
}

std::wstring FormatCountTr(uint64_t value) {
    const std::wstring digits = std::to_wstring(value);
    std::wstring text;
    text.reserve(digits.size() + digits.size() / 3);
    const size_t n = digits.size();
    const wchar_t separator = UiTurkish() ? L'.' : L',';
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && (n - i) % 3 == 0) text.push_back(separator);
        text.push_back(digits[i]);
    }
    return text;
}

std::wstring FormatSizeText(const Mesh& mesh) {
    return L"X " + FormatDecimalTr(mesh.size[0], 1) + L" × Y " + FormatDecimalTr(mesh.size[1], 1) +
           L" × Z " + FormatDecimalTr(mesh.size[2], 1) + L" mm";
}

std::wstring FormatSizeClipboardText(const Mesh& mesh) {
    return FormatDecimalTr(mesh.size[0], 1) + L" x " + FormatDecimalTr(mesh.size[1], 1) + L" x " +
           FormatDecimalTr(mesh.size[2], 1) + L" mm";
}

double MeshVolumeCm3(const Mesh& mesh) {
    return mesh.volumeMm3 < 0 ? -1.0 : mesh.volumeMm3 / 1000.0;
}

double MeshSurfaceAreaCm2(const Mesh& mesh) {
    return mesh.surfaceAreaMm2 < 0 ? -1.0 : mesh.surfaceAreaMm2 / 100.0;
}

double MeshSolidMassGrams(const Mesh& mesh, double densityGcm3) {
    const double cm3 = MeshVolumeCm3(mesh);
    return cm3 < 0 ? -1.0 : cm3 * densityGcm3;
}

double MeshTypicalPrintMassGrams(const Mesh& mesh) {
    if (mesh.volumeMm3 < 0) return -1.0;
    double shell;
    if (mesh.printShellMm3 >= 0) {
        shell = std::min(mesh.volumeMm3, mesh.printShellMm3);
    } else {
        if (mesh.surfaceAreaMm2 < 0) return MeshSolidMassGrams(mesh);
        shell = std::min(mesh.volumeMm3, mesh.surfaceAreaMm2 * kTypicalShellMm);
    }
    const double printedMm3 = shell + kTypicalInfill * (mesh.volumeMm3 - shell);
    return printedMm3 / 1000.0 * kTypicalFilamentDensity;
}

std::wstring FormatMassTr(double grams) {
    if (!(grams >= 0)) return L"?";
    if (grams < 9.95) return FormatDecimalTr(grams, 1) + L" g";
    if (grams < 999.5) return FormatDecimalTr(grams, 0) + L" g";
    return FormatDecimalTr(grams / 1000.0, 2) + L" kg";
}

const wchar_t* ModelFormatName(ModelFormat format) {
    switch (format) {
    case ModelFormat::Stl:
        return L"STL";
    case ModelFormat::ThreeMf:
        return L"3MF";
    default:
        return L"?";
    }
}

// ---------------------------------------------------------------------------------------------
// Mass estimate self-test
// ---------------------------------------------------------------------------------------------

namespace {

// Axis-aligned box, outward-facing winding (inward = true: inward-facing, for a cavity).
void AppendBox(std::vector<float>& raw, const double lo[3], const double hi[3], bool inward) {
    const double v[8][3] = {{lo[0], lo[1], lo[2]}, {hi[0], lo[1], lo[2]}, {hi[0], hi[1], lo[2]}, {lo[0], hi[1], lo[2]},
                            {lo[0], lo[1], hi[2]}, {hi[0], lo[1], hi[2]}, {hi[0], hi[1], hi[2]}, {lo[0], hi[1], hi[2]}};
    static const int f[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                                 {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    for (const auto& face : f) {
        const int order[3] = {face[0], inward ? face[2] : face[1], inward ? face[1] : face[2]};
        for (int i : order)
            for (int k = 0; k < 3; ++k) raw.push_back(static_cast<float>(v[i][k]));
    }
}

}  // namespace

bool ModelMassSelfTest(std::wstring& report) {
    struct Case {
        const wchar_t* name;
        double lo[3], hi[3];
        double innerLo[3], innerHi[3];   // innerHi[0] <= innerLo[0]: no cavity
        double minGrams, maxGrams;
    };
    // The ranges are for kTypicalInfill = 20% (the test should fail if infill changes; at 15% the cube gives
    // 3.58 g, the plate 21.29 g).
    // Plate: 0.8 mm bottom + 0.8 mm top shell = 80% of 2 mm, the remaining 20% × 20% infill -> ~84% of solid mass.
    // Box: 1 mm wall < 2 × shell -> walls print solid (1.24 × 2.168 cm³ = 2.69 g).
    const Case cases[] = {
        {L"20 mm solid cube", {0, 0, 0}, {20, 20, 20}, {0, 0, 0}, {0, 0, 0}, 3.85, 4.05},
        {L"100x100x2 mm plate", {0, 0, 0}, {100, 100, 2}, {0, 0, 0}, {0, 0, 0}, 21.3, 21.7},
        {L"20 mm box, 1 mm walls", {0, 0, 0}, {20, 20, 20}, {1, 1, 1}, {19, 19, 19}, 2.55, 2.70},
    };
    bool ok = true;
    report.clear();
    for (const Case& c : cases) {
        std::vector<float> raw;
        AppendBox(raw, c.lo, c.hi, false);
        if (c.innerHi[0] > c.innerLo[0]) AppendBox(raw, c.innerLo, c.innerHi, true);
        Mesh mesh;
        std::wstring error;
        const bool built = BuildMeshFromTriangles(raw, ModelFormat::Stl, 0, kModelLoadSkipFeatureEdges, mesh, error,
                                                  LoadDeadline(0));
        const double grams = built ? MeshTypicalPrintMassGrams(mesh) : -1.0;
        const bool pass = built && mesh.printShellMm3 >= 0 && grams >= c.minGrams && grams <= c.maxGrams;
        ok = ok && pass;
        wchar_t line[200];
        swprintf_s(line, L"%s %s: %.2f g (expected %.2f..%.2f, solid %.2f g)\r\n", pass ? L"OK" : L"FAIL", c.name,
                   grams, c.minGrams, c.maxGrams, built ? MeshSolidMassGrams(mesh) : -1.0);
        report += line;
    }
    return ok;
}
