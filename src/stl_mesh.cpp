#include "stl_mesh.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <new>
#include <string_view>

#include "model_internal.h"

namespace {
constexpr size_t kBinaryHeaderSize = 84;
constexpr size_t kBinaryTriangleSize = 50;

// Does the triangle count in a binary STL header match the file size (84 + 50 × n; a few extra trailing bytes
// are allowed)? In an ASCII file bytes 80..83 are text, so the count comes out huge and does not match
// (binary headers that start with "solid" are thus recognized correctly too).
bool BinarySizeMatches(const uint8_t* data, size_t size) {
    if (size < kBinaryHeaderSize + kBinaryTriangleSize) return false;
    uint32_t count = 0;
    std::memcpy(&count, data + 80, sizeof(count));
    if (count == 0) return false;
    const uint64_t expected = kBinaryHeaderSize + static_cast<uint64_t>(count) * kBinaryTriangleSize;
    return size >= expected && size - expected < 2 * kBinaryTriangleSize;
}

// Any non-text byte (NUL or a control character other than whitespace) in the first 64 KB?
bool HasBinaryBytes(const uint8_t* data, size_t size) {
    const size_t probeSize = std::min<size_t>(size, 64 * 1024);
    for (size_t i = 0; i < probeSize; ++i) {
        const uint8_t c = data[i];
        if (c < 0x20 && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != '\v') return true;
    }
    return false;
}

// If the header count exceeds the file, it is clamped to the bytes available (size-consistent binary or last resort).
bool ParseBinary(const uint8_t* data, size_t size, std::vector<float>& out) {
    if (size < kBinaryHeaderSize) return false;

    uint32_t count = 0;
    std::memcpy(&count, data + 80, sizeof(count));
    const size_t available = (size - kBinaryHeaderSize) / kBinaryTriangleSize;
    if (count == 0 || available == 0) return false;
    count = static_cast<uint32_t>(std::min<size_t>(count, available));

    out.resize(static_cast<size_t>(count) * 9);
    const uint8_t* p = data + kBinaryHeaderSize;
    for (uint32_t i = 0; i < count; ++i, p += kBinaryTriangleSize) {
        std::memcpy(&out[static_cast<size_t>(i) * 9], p + 12, 36);
    }
    return true;
}

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// Reads a number (leading whitespace and ',' are skipped). If the number is not followed by whitespace, ',' or
// end of file ("1.5abc"), or there is no number, returns false and p does not advance (the unreadable part is
// skipped as the next word).
bool ReadNumber(const char*& p, const char* end, float& value) {
    const char* q = p;
    while (q < end && (IsSpace(*q) || *q == ',')) ++q;
    if (q < end && *q == '+') ++q;
    const auto result = std::from_chars(q, end, value);
    if (result.ec != std::errc()) return false;
    if (result.ptr < end && (*result.ptr == 'D' || *result.ptr == 'd')) {
        // Fortran exponent ("1.000000D+01"): 'D' -> 'e' in a short copy, then parsed again.
        const char* tokenEnd = result.ptr;
        while (tokenEnd < end && !IsSpace(*tokenEnd) && *tokenEnd != ',') ++tokenEnd;
        char buffer[64];
        const size_t length = static_cast<size_t>(tokenEnd - q);
        if (length >= sizeof(buffer)) return false;
        std::memcpy(buffer, q, length);
        buffer[result.ptr - q] = 'e';
        const auto retry = std::from_chars(buffer, buffer + length, value);
        if (retry.ec != std::errc() || retry.ptr != buffer + length) return false;
        p = tokenEnd;
        return true;
    }
    if (result.ptr < end && !IsSpace(*result.ptr) && *result.ptr != ',') return false;
    p = result.ptr;
    return true;
}

// Case-insensitive keyword comparison (some exporters write "FACET", "Vertex").
bool KeywordIs(std::string_view token, std::string_view keyword) {
    if (token.size() != keyword.size()) return false;
    for (size_t i = 0; i < token.size(); ++i) {
        char c = token[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != keyword[i]) return false;
    }
    return true;
}

// ASCII STL: "facet normal ... outer loop  vertex x y z (×3)  endloop endfacet". Triangles are read per facet
// block: if a block does not have exactly 3 valid vertices (missing/extra vertex, unreadable number), only that
// facet is skipped and later facets do not shift. Vertices outside any "facet" (broken files with no facet
// structure) are grouped in threes in order, as before. An unfinished last facet (no endfacet) is kept if it has 3.
bool ParseAscii(const uint8_t* data, size_t size, std::vector<float>& out, const LoadDeadline& deadline) {
    const char* begin = reinterpret_cast<const char*>(data);
    const char* end = begin + size;

    std::vector<float> positions;
    float facet[9]{};
    int facetVertices = 0;
    bool inFacet = false;
    bool facetBad = false;
    float loose[9]{};
    int looseVertices = 0;

    auto finishFacet = [&]() {
        if (inFacet && !facetBad && facetVertices == 3) positions.insert(positions.end(), facet, facet + 9);
        inFacet = false;
        facetBad = false;
        facetVertices = 0;
    };

    const char* p = begin;
    uint32_t tokens = 0;
    while (p < end) {
        if ((++tokens & 0xFFFF) == 0) ThrowIfExpired(deadline);
        while (p < end && IsSpace(*p)) ++p;
        const char* tokenStart = p;
        while (p < end && !IsSpace(*p)) ++p;
        const std::string_view token(tokenStart, static_cast<size_t>(p - tokenStart));
        if (token.empty()) break;

        if (KeywordIs(token, "facet")) {
            finishFacet();  // previous facet is missing its "endfacet"
            inFacet = true;
            looseVertices = 0;
        } else if (KeywordIs(token, "endfacet")) {
            finishFacet();
        } else if (KeywordIs(token, "vertex")) {
            float v[3]{};
            const bool ok = ReadNumber(p, end, v[0]) && ReadNumber(p, end, v[1]) && ReadNumber(p, end, v[2]);
            if (inFacet) {
                if (!ok || facetVertices >= 3) {
                    facetBad = true;
                } else {
                    std::memcpy(&facet[facetVertices * 3], v, sizeof(v));
                    ++facetVertices;
                }
            } else if (ok) {
                std::memcpy(&loose[looseVertices * 3], v, sizeof(v));
                if (++looseVertices == 3) {
                    positions.insert(positions.end(), loose, loose + 9);
                    looseVertices = 0;
                }
            } else {
                looseVertices = 0;
            }
        }
        // Other words (solid, normal and its numbers, outer, loop, endloop, endsolid, name) are ignored.
    }
    finishFacet();

    const size_t triangleCount = positions.size() / 9;
    if (triangleCount == 0) return false;
    positions.resize(triangleCount * 9);
    out.swap(positions);
    return true;
}
}  // namespace

bool ParseStlTriangles(const uint8_t* data, size_t size, std::vector<float>& raw, std::wstring& error,
                       bool& lenient, const LoadDeadline& deadline) {
    raw.clear();
    lenient = false;
    if (!data || size < 15) {
        error = L"Dosya boş veya çok küçük";
        return false;
    }

    // 1. Binary if the header count matches the size. 2. Otherwise ASCII (keywords are case-insensitive,
    // no "solid" line is required). 3. If ASCII yields no triangles and the file has non-text bytes, last resort:
    // truncated binary read (cut/corrupt file). This last path is suspicious (lenient: 100 m size check).
    bool parsed = false;
    if (BinarySizeMatches(data, size)) parsed = ParseBinary(data, size, raw);
    if (!parsed) {
        raw.clear();
        parsed = ParseAscii(data, size, raw, deadline);
    }
    if (!parsed && HasBinaryBytes(data, size)) {
        raw.clear();
        parsed = ParseBinary(data, size, raw);
        lenient = parsed;
    }
    if (!parsed || raw.empty()) {
        std::vector<float>().swap(raw);
        lenient = false;
        error = L"STL çözümlenemedi: üçgen bulunamadı";
        return false;
    }
    return true;
}

bool LoadStlWithDeadline(const void* data, size_t size, uint32_t maxTriangles, Mesh& out, std::wstring& error,
                         uint32_t flags, const LoadDeadline& deadline) {
    try {
        out = Mesh{};
        error.clear();

        std::vector<float> raw;
        bool lenient = false;
        if (!ParseStlTriangles(static_cast<const uint8_t*>(data), size, raw, error, lenient, deadline)) {
            out = Mesh{};
            return false;
        }
        if (!BuildMeshFromTriangles(raw, ModelFormat::Stl, maxTriangles, flags, out, error, deadline, lenient)) {
            out = Mesh{};
            return false;
        }
        out.objectCount = 1;
        return true;
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

bool LoadStl(const void* data, size_t size, uint32_t maxTriangles, Mesh& out, std::wstring& error, uint32_t flags) {
    return LoadStlWithDeadline(data, size, maxTriangles, out, error, flags, LoadDeadline(kPreviewLoadBudgetMs));
}
