// model_3mf.cpp — 3MF (OPC/zip package) reader. Windows SDK only: msopc (OPC), XmlLite, shlwapi.
//
// Reads: <model unit>, <object id><mesh><vertices><vertex x y z/>, <triangles><triangle v1 v2 v3/>,
// <components><component objectid p:path transform/>, <build><item objectid p:path transform printable/>.
// Transform: 3x4 row-vector convention (p' = p * M). Output: raw triangle soup in original space (mm).

#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <shlwapi.h>
#include <msopc.h>
#include <xmllite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "model_internal.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t k3mfModelRelationshipType[] = L"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel";
// Memory budget: the preview draws at most 4M triangles; stats use the full set up to 8M triangles.
// 8M output triangles = 288 MB of raw data; 8M vertices / triangles = two arrays of ~96 MB.
constexpr uint64_t kMaxTotalVertices = 8'000'000;
constexpr uint64_t kMaxTotalTriangleIndices = 8'000'000;  // number of parsed <triangle> elements
constexpr size_t kMaxOutputTriangles = 8'000'000;          // including instancing
constexpr uint64_t kMaxEmitCalls = 1'000'000;  // guards against exponential blow-up in component trees
constexpr int kMaxComponentDepth = 16;
// Number of <object>/<component>/<item> elements stored per package (same order as kMaxEmitCalls:
// more could not be drawn anyway). Guards against memory inflation via compressed repetitive XML.
constexpr uint64_t kMaxStoredNodes = 1'000'000;
constexpr size_t kMaxPathLength = 1024;  // component/item path attribute (characters)
// Decompressed XML byte budget and time limit, shared by all parts in the package.
// Guards against zip bombs and XmlLite's quadratic slowdown on very long attribute values.
constexpr uint64_t kMaxXmlBytes = 512ull << 20;  // 512 MB
// Limit on bytes read with no '<' in between. An attribute value cannot contain '<', and 3MF has no long
// text/whitespace either. Since XmlLite buffers a single long value in memory (converted to UTF-16), this
// stops far earlier than the byte budget.
constexpr uint64_t kMaxXmlRunBytes = 1ull << 20;  // 1 MB (XmlLite: 4 MB value ~0.6 s, quadratic growth)
constexpr ULONGLONG kMaxParseMs = 20'000;          // in addition to LoadModel's time budget (LoadDeadline)
// Zip directory limits: msopc allocates memory for every entry when opening the package (~2.6 KB per entry).
constexpr uint64_t kMaxZipEntries = 4096;
constexpr uint64_t kMaxZipCentralDirectoryBytes = 8ull << 20;  // 8 MB

// 3x4 affine transform, row-major: m[0..2] = row 1, ..., m[9..11] = translation row.
struct Transform {
    double m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

    void Apply(double x, double y, double z, double& ox, double& oy, double& oz) const {
        ox = x * m[0] + y * m[3] + z * m[6] + m[9];
        oy = x * m[1] + y * m[4] + z * m[7] + m[10];
        oz = x * m[2] + y * m[5] + z * m[8] + m[11];
    }

    double Determinant() const {
        return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
               m[2] * (m[3] * m[7] - m[4] * m[6]);
    }
};

// child * parent: child is applied first, then parent.
Transform Multiply(const Transform& child, const Transform& parent) {
    Transform r;
    const double* a = child.m;
    const double* b = parent.m;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 3; ++j) {
            double v = a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j] + a[i * 3 + 2] * b[2 * 3 + j];
            if (i == 3) v += b[9 + j];
            r.m[i * 3 + j] = v;
        }
    }
    return r;
}

struct Component {
    uint32_t objectId = 0;
    std::wstring path;  // normalized; empty means the same document
    Transform transform;
};

struct Object {
    std::vector<float> verts;   // x y z
    std::vector<uint32_t> tris;  // v1 v2 v3
    std::vector<Component> comps;
};

struct BuildItem {
    uint32_t objectId = 0;
    std::wstring path;
    Transform transform;
};

// double -> float: an out-of-range value (would be UB) becomes NaN; BuildMeshFromTriangles drops that triangle.
float ToFloat(double v) {
    if (!std::isfinite(v) || std::fabs(v) > static_cast<double>(FLT_MAX)) return std::numeric_limits<float>::quiet_NaN();
    return static_cast<float>(v);
}

struct ModelDoc {
    double unitScale = 1.0;
    std::unordered_map<uint32_t, Object> objects;
    std::vector<uint32_t> objectOrder;  // document order (for the fallback path)
    std::vector<BuildItem> items;      // printable items only (printable != "0")
};

std::wstring HrMessage(const wchar_t* format, HRESULT hr) {
    wchar_t buffer[160];
    swprintf_s(buffer, format, static_cast<unsigned int>(hr));
    return buffer;
}

// ---------------------------------------------------------------------------------------------
// Number parsing (locale-independent)
// ---------------------------------------------------------------------------------------------

// Copies a wide-character number into a small char buffer. false on failure.
bool ToAsciiNumber(const wchar_t* text, size_t length, char (&buffer)[64], size_t& outLength) {
    size_t i = 0;
    while (i < length && std::iswspace(text[i])) ++i;
    if (i < length && text[i] == L'+') ++i;
    size_t n = 0;
    for (; i < length; ++i) {
        const wchar_t c = text[i];
        if (std::iswspace(c)) break;
        if (c > 0x7F || n >= 63) return false;
        buffer[n++] = static_cast<char>(c);
    }
    buffer[n] = 0;
    outLength = n;
    return n > 0;
}

bool ParseDouble(const wchar_t* text, size_t length, double& value) {
    char buffer[64];
    size_t n = 0;
    if (!ToAsciiNumber(text, length, buffer, n)) return false;
    double v = 0;
    const auto result = std::from_chars(buffer, buffer + n, v);
    if (result.ec != std::errc() || result.ptr != buffer + n) return false;
    value = v;
    return true;
}

bool ParseUint32(const wchar_t* text, size_t length, uint32_t& value) {
    char buffer[64];
    size_t n = 0;
    if (!ToAsciiNumber(text, length, buffer, n)) return false;
    uint32_t v = 0;
    const auto result = std::from_chars(buffer, buffer + n, v);
    if (result.ec != std::errc() || result.ptr != buffer + n) return false;
    value = v;
    return true;
}

Transform ParseTransform(const wchar_t* text, size_t length) {
    Transform t;
    double values[12];
    int count = 0;
    size_t i = 0;
    while (i < length && count < 12) {
        while (i < length && std::iswspace(text[i])) ++i;
        if (i >= length) break;
        const size_t start = i;
        while (i < length && !std::iswspace(text[i])) ++i;
        double v = 0;
        if (!ParseDouble(text + start, i - start, v) || !std::isfinite(v)) return Transform{};
        values[count++] = v;
    }
    if (count < 12) return Transform{};
    for (int k = 0; k < 12; ++k) t.m[k] = values[k];
    return t;
}

// ---------------------------------------------------------------------------------------------
// Part names
// ---------------------------------------------------------------------------------------------

std::wstring NormalizePartName(std::wstring name) {
    if (name.empty()) return name;
    if (name[0] != L'/') name.insert(name.begin(), L'/');
    if (name.find(L'%') != std::wstring::npos) {
        DWORD flags = URL_UNESCAPE_INPLACE;
#ifdef URL_UNESCAPE_AS_UTF8
        flags |= URL_UNESCAPE_AS_UTF8;
#endif
        if (SUCCEEDED(UrlUnescapeW(&name[0], nullptr, nullptr, flags))) {
            name.resize(std::wcslen(name.c_str()));
        }
    }
    for (wchar_t& c : name) c = static_cast<wchar_t>(std::towlower(c));
    return name;
}

bool EndsWith(const std::wstring& text, const wchar_t* suffix) {
    const size_t n = std::wcslen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

struct BstrHolder {
    BSTR value = nullptr;
    ~BstrHolder() {
        if (value) SysFreeString(value);
    }
};

struct CoTaskString {
    LPWSTR value = nullptr;
    ~CoTaskString() {
        if (value) CoTaskMemFree(value);
    }
};

// ---------------------------------------------------------------------------------------------
// Budgeted stream: given to XmlLite in place of the part stream
// ---------------------------------------------------------------------------------------------

struct ReadBudget {
    uint64_t bytes = 0;
    uint64_t run = 0;   // bytes read since the last '<'
    ULONGLONG deadline = 0;
    const LoadDeadline* load = nullptr;
    bool exceeded = false;
    bool timedOut = false;
};

// Read() returns E_ABORT when the budget or time runs out. Since XmlLite also calls Read() while scanning a long
// attribute value, this also cuts long Read() calls that no check in the loop body could catch.
class LimitedStream final : public ISequentialStream {
public:
    LimitedStream(ComPtr<IStream> inner, ReadBudget& budget) : m_inner(std::move(inner)), m_budget(budget) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ISequentialStream) {
            *ppv = static_cast<ISequentialStream*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG refs = --m_refs;
        if (refs == 0) delete this;
        return refs;
    }

    IFACEMETHODIMP Read(void* buffer, ULONG size, ULONG* read) override {
        if (read) *read = 0;
        if (m_budget.exceeded) return E_ABORT;
        if (GetTickCount64() > m_budget.deadline || (m_budget.load && m_budget.load->Expired())) {
            m_budget.exceeded = true;
            m_budget.timedOut = true;
            return E_ABORT;
        }
        if (m_budget.bytes > kMaxXmlBytes || m_budget.run > kMaxXmlRunBytes) {
            m_budget.exceeded = true;
            return E_ABORT;
        }
        ULONG got = 0;
        const HRESULT hr = m_inner->Read(buffer, size, &got);
        m_budget.bytes += got;
        const uint8_t* bytes = static_cast<const uint8_t*>(buffer);
        ULONG i = got;
        while (i > 0 && bytes[i - 1] != '<') --i;   // '<' is byte 0x3C in both UTF-8 and UTF-16
        m_budget.run = i > 0 ? got - i : m_budget.run + got;
        if (read) *read = got;
        return hr;
    }
    IFACEMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }

private:
    ComPtr<IStream> m_inner;
    ReadBudget& m_budget;
    ULONG m_refs = 1;
};

// ---------------------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------------------

// Little-endian read (bounds checked by the caller).
uint16_t Rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t Rd64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

// Checks the zip directory from the in-memory bytes before handing it to OPC: entry count / directory size from
// the end-of-directory record (zip64 if needed) and the number of central directory header signatures in the
// file. Since msopc allocates memory per entry, packages with very many (empty) entries are rejected. If no
// end-of-directory record is found, the decision is left to OPC.
bool CheckZipDirectory(const uint8_t* data, size_t size, std::wstring& error) {
    constexpr uint32_t kEocdSig = 0x06054b50;
    constexpr uint32_t kZip64LocatorSig = 0x07064b50;
    constexpr uint32_t kZip64EocdSig = 0x06064b50;
    constexpr size_t kEocdSize = 22;
    constexpr size_t kZip64EocdSize = 56;
    constexpr size_t kZip64LocatorSize = 20;
    const auto reject = [&error]() {
        error = L"3MF paketi çok fazla dosya içeriyor";
        return false;
    };

    // Number of central directory header signatures ("PK\x01\x02"): whichever end-of-directory record is used,
    // every entry has a header.
    uint64_t headers = 0;
    for (const uint8_t* p = data; size >= 4 && p <= data + size - 4;) {
        p = static_cast<const uint8_t*>(std::memchr(p, 'P', static_cast<size_t>(data + size - 3 - p)));
        if (!p) break;
        if (p[1] == 'K' && p[2] == 0x01 && p[3] == 0x02 && ++headers > kMaxZipEntries) return reject();
        ++p;
    }

    if (size < kEocdSize) return true;
    const size_t lowest = size - kEocdSize > 0xFFFF ? size - kEocdSize - 0xFFFF : 0;
    for (size_t pos = size - kEocdSize + 1; pos-- > lowest;) {
        if (Rd32(data + pos) != kEocdSig) continue;
        const uint8_t* e = data + pos;
        uint64_t entries = std::max(Rd16(e + 8), Rd16(e + 10));
        uint64_t directoryBytes = Rd32(e + 12);
        if ((entries == 0xFFFF || directoryBytes == 0xFFFFFFFFu || Rd32(e + 16) == 0xFFFFFFFFu) &&
            pos >= kZip64LocatorSize && Rd32(e - kZip64LocatorSize) == kZip64LocatorSig) {
            const uint64_t offset = Rd64(e - kZip64LocatorSize + 8);
            if (size >= kZip64EocdSize && offset <= size - kZip64EocdSize && Rd32(data + offset) == kZip64EocdSig) {
                const uint8_t* z = data + offset;
                entries = std::max(Rd64(z + 24), Rd64(z + 32));
                directoryBytes = Rd64(z + 40);
            }
        }
        if (entries > kMaxZipEntries || directoryBytes > kMaxZipCentralDirectoryBytes) return reject();
        return true;
    }
    return true;
}

class ThreeMfReader {
public:
    ThreeMfReader(std::vector<float>& raw, std::wstring& error, const LoadDeadline& deadline)
        : m_raw(raw), m_error(error), m_deadline(deadline) {
        m_budget.deadline = GetTickCount64() + kMaxParseMs;
        m_budget.load = &deadline;
    }

    bool Read(const uint8_t* data, size_t size, uint32_t& objectCount) {
        if (!CheckZipDirectory(data, size, m_error)) return false;

        HRESULT hr = CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&m_factory));
        if (FAILED(hr)) return Fail(L"3MF paketi açılamadı (0x%08X)", hr);

        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(data, static_cast<UINT>(size)));
        if (!stream) return Fail(L"3MF paketi açılamadı (0x%08X)", E_OUTOFMEMORY);

        hr = m_factory->ReadPackageFromStream(stream.Get(), OPC_CACHE_ON_ACCESS, &m_package);
        if (FAILED(hr)) {
            m_package.Reset();
            LARGE_INTEGER zero{};
            stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            hr = m_factory->ReadPackageFromStream(stream.Get(), OPC_READ_DEFAULT, &m_package);
            if (FAILED(hr)) return Fail(L"3MF paketi açılamadı (0x%08X)", hr);
        }
        if (m_deadline.Expired()) return TimedOut();

        if (!BuildPartTable()) return false;

        std::wstring rootName;
        if (!FindRootModel(rootName)) return false;

        ModelDoc* root = GetDoc(rootName);
        if (!root) {
            if (m_error.empty()) m_error = L"3MF: model parçası bulunamadı";
            return false;
        }
        m_rootScale = root->unitScale;

        // Roots to draw: build items; if none (fallback), every root object that is not a component of another.
        struct Root {
            ModelDoc* doc;
            uint32_t objectId;
            Transform transform;
        };
        std::vector<Root> roots;
        for (const BuildItem& item : root->items) {
            ModelDoc* doc = root;
            if (!item.path.empty()) {
                doc = GetDoc(item.path);
                if (!m_error.empty()) return false;
                if (!doc) continue;
            }
            roots.push_back(Root{doc, item.objectId, item.transform});
        }
        if (root->items.empty()) {
            std::unordered_set<uint32_t> referenced;
            for (const auto& entry : root->objects) {
                for (const Component& comp : entry.second.comps) {
                    if (comp.path.empty() || comp.path == rootName) referenced.insert(comp.objectId);
                }
            }
            for (uint32_t id : root->objectOrder) {
                if (referenced.count(id) == 0) roots.push_back(Root{root, id, Transform{}});
            }
        }

        // Counting pass (same tree and limits as Emit): if the output budget is exceeded, reject without
        // allocating; otherwise allocate the raw array once, at full size.
        uint64_t total = 0;
        for (const Root& r : roots) {
            if (!Count(*r.doc, r.objectId, 0, total)) return false;
        }
        m_raw.reserve(static_cast<size_t>(total) * 9);

        uint32_t emittedItems = 0;
        for (const Root& r : roots) {
            const size_t before = m_raw.size();
            if (!Emit(*r.doc, r.objectId, r.transform, 0)) return false;
            if (m_raw.size() > before) ++emittedItems;
        }

        if (m_raw.empty()) {
            m_error = L"3MF: yazdırılacak nesne bulunamadı";
            return false;
        }
        objectCount = emittedItems;
        return true;
    }

private:
    bool Fail(const wchar_t* format, HRESULT hr) {
        m_error = HrMessage(format, hr);
        return false;
    }

    bool TimedOut() {
        m_error = kLoadTimeoutMessage;
        return false;
    }

    bool BuildPartTable() {
        ComPtr<IOpcPartSet> parts;
        HRESULT hr = m_package->GetPartSet(&parts);
        if (FAILED(hr)) return Fail(L"3MF paketi açılamadı (0x%08X)", hr);
        ComPtr<IOpcPartEnumerator> enumerator;
        hr = parts->GetEnumerator(&enumerator);
        if (FAILED(hr)) return Fail(L"3MF paketi açılamadı (0x%08X)", hr);

        for (;;) {
            BOOL hasNext = FALSE;
            hr = enumerator->MoveNext(&hasNext);
            if (FAILED(hr)) return Fail(L"3MF paketi açılamadı (0x%08X)", hr);
            if (!hasNext) break;
            ComPtr<IOpcPart> part;
            if (FAILED(enumerator->GetCurrent(&part)) || !part) continue;
            ComPtr<IOpcPartUri> uri;
            if (FAILED(part->GetName(&uri)) || !uri) continue;
            BstrHolder raw;
            if (FAILED(uri->GetRawUri(&raw.value)) || !raw.value) continue;
            std::wstring name = NormalizePartName(std::wstring(raw.value, SysStringLen(raw.value)));
            if (name.empty()) continue;
            m_partOrder.push_back(name);
            m_parts[name] = part;
        }
        return true;
    }

    bool FindRootModel(std::wstring& rootName) {
        ComPtr<IOpcRelationshipSet> relationships;
        if (SUCCEEDED(m_package->GetRelationshipSet(&relationships)) && relationships) {
            ComPtr<IOpcRelationshipEnumerator> enumerator;
            if (SUCCEEDED(relationships->GetEnumerator(&enumerator)) && enumerator) {
                for (;;) {
                    BOOL hasNext = FALSE;
                    if (FAILED(enumerator->MoveNext(&hasNext)) || !hasNext) break;
                    ComPtr<IOpcRelationship> relationship;
                    if (FAILED(enumerator->GetCurrent(&relationship)) || !relationship) continue;
                    CoTaskString type;
                    if (FAILED(relationship->GetRelationshipType(&type.value)) || !type.value) continue;
                    if (std::wcscmp(type.value, k3mfModelRelationshipType) != 0) continue;
                    ComPtr<IUri> target;
                    if (FAILED(relationship->GetTargetUri(&target)) || !target) continue;
                    BstrHolder raw;
                    if (FAILED(target->GetRawUri(&raw.value)) || !raw.value) continue;
                    const std::wstring name =
                        NormalizePartName(std::wstring(raw.value, SysStringLen(raw.value)));
                    if (m_parts.count(name) != 0) {
                        rootName = name;
                        return true;
                    }
                }
            }
        }

        if (m_parts.count(L"/3d/3dmodel.model") != 0) {
            rootName = L"/3d/3dmodel.model";
            return true;
        }
        for (const std::wstring& name : m_partOrder) {
            if (EndsWith(name, L".model")) {
                rootName = name;
                return true;
            }
        }
        m_error = L"3MF: model parçası bulunamadı";
        return false;
    }

    // Returns the document (parsing it if needed). If the part is missing: nullptr and m_error stays empty;
    // on a parse error: nullptr and m_error is set.
    ModelDoc* GetDoc(const std::wstring& normalizedName) {
        auto cached = m_docs.find(normalizedName);
        if (cached != m_docs.end()) return &cached->second;
        if (m_missing.count(normalizedName) != 0) return nullptr;

        auto part = m_parts.find(normalizedName);
        if (part == m_parts.end()) {
            m_missing.insert(normalizedName);
            return nullptr;
        }
        ModelDoc doc;
        if (!ParseDoc(part->second.Get(), doc)) return nullptr;
        auto inserted = m_docs.emplace(normalizedName, std::move(doc));
        return &inserted.first->second;
    }

    bool ParseDoc(IOpcPart* part, ModelDoc& doc) {
        ComPtr<IStream> stream;
        HRESULT hr = part->GetContentStream(&stream);
        if (FAILED(hr) || !stream) return Fail(L"3MF: XML okunamadı (0x%08X)", FAILED(hr) ? hr : E_POINTER);

        ComPtr<IXmlReader> reader;
        hr = CreateXmlReader(IID_PPV_ARGS(&reader), nullptr);
        if (FAILED(hr)) return Fail(L"3MF: XML okunamadı (0x%08X)", hr);
        reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
        reader->SetProperty(XmlReaderProperty_MaxElementDepth, 64);
        ComPtr<ISequentialStream> limited;
        limited.Attach(new LimitedStream(stream, m_budget));
        hr = reader->SetInput(limited.Get());
        if (FAILED(hr)) return Fail(L"3MF: XML okunamadı (0x%08X)", hr);

        Object* current = nullptr;
        bool inMesh = false;
        bool inBuild = false;

        for (;;) {
            XmlNodeType type = XmlNodeType_None;
            hr = reader->Read(&type);
            if (hr == S_FALSE) break;
            if (hr != S_OK) {
                if (m_budget.timedOut) return TimedOut();
                if (m_budget.exceeded) {
                    m_error = L"3MF: model çok büyük ya da çözümlemesi çok uzun sürdü";
                    return false;
                }
                return Fail(L"3MF: XML okunamadı (0x%08X)", hr);
            }

            if (type == XmlNodeType_EndElement) {
                const wchar_t* name = nullptr;
                if (FAILED(reader->GetLocalName(&name, nullptr)) || !name) continue;
                if (std::wcscmp(name, L"object") == 0) {
                    current = nullptr;
                    inMesh = false;
                } else if (std::wcscmp(name, L"mesh") == 0) {
                    inMesh = false;
                } else if (std::wcscmp(name, L"build") == 0) {
                    inBuild = false;
                }
                continue;
            }
            if (type != XmlNodeType_Element) continue;

            const wchar_t* name = nullptr;
            if (FAILED(reader->GetLocalName(&name, nullptr)) || !name) continue;
            const bool empty = reader->IsEmptyElement() != FALSE;

            if (std::wcscmp(name, L"vertex") == 0) {
                if (!current || !inMesh) continue;
                if (++m_totalVertices > kMaxTotalVertices) {
                    m_error = L"3MF: model çok büyük";
                    return false;
                }
                double xyz[3] = {0, 0, 0};
                ForEachAttribute(reader.Get(), [&](const wchar_t* attr, const wchar_t* value, UINT length) {
                    int axis = -1;
                    if (attr[0] != 0 && attr[1] == 0) {
                        if (attr[0] == L'x') axis = 0;
                        else if (attr[0] == L'y') axis = 1;
                        else if (attr[0] == L'z') axis = 2;
                    }
                    if (axis < 0) return;
                    double v = 0;
                    if (!ParseDouble(value, length, v) || !std::isfinite(v)) v = 0;
                    xyz[axis] = v;
                });
                current->verts.push_back(ToFloat(xyz[0]));
                current->verts.push_back(ToFloat(xyz[1]));
                current->verts.push_back(ToFloat(xyz[2]));
            } else if (std::wcscmp(name, L"triangle") == 0) {
                if (!current || !inMesh) continue;
                if (++m_totalTriangles > kMaxTotalTriangleIndices) {
                    m_error = L"3MF: model çok büyük";
                    return false;
                }
                uint32_t v[3] = {0, 0, 0};
                int found = 0;
                bool bad = false;
                ForEachAttribute(reader.Get(), [&](const wchar_t* attr, const wchar_t* value, UINT length) {
                    int index = -1;
                    if (attr[0] == L'v' && attr[1] >= L'1' && attr[1] <= L'3' && attr[2] == 0) index = attr[1] - L'1';
                    if (index < 0) return;
                    if (!ParseUint32(value, length, v[index])) bad = true;
                    ++found;
                });
                if (bad || found != 3) continue;
                current->tris.push_back(v[0]);
                current->tris.push_back(v[1]);
                current->tris.push_back(v[2]);
            } else if (std::wcscmp(name, L"mesh") == 0) {
                if (current && !empty) inMesh = true;
            } else if (std::wcscmp(name, L"object") == 0) {
                uint32_t id = 0;
                bool hasId = false;
                ForEachAttribute(reader.Get(), [&](const wchar_t* attr, const wchar_t* value, UINT length) {
                    if (std::wcscmp(attr, L"id") == 0) hasId = ParseUint32(value, length, id);
                });
                current = nullptr;
                inMesh = false;
                if (!hasId) continue;  // invalid object: its content is ignored
                auto it = doc.objects.find(id);
                if (it == doc.objects.end()) {
                    if (!CountNode()) return false;
                    it = doc.objects.emplace(id, Object{}).first;
                    doc.objectOrder.push_back(id);
                } else {
                    it->second = Object{};
                }
                if (!empty) current = &it->second;
            } else if (std::wcscmp(name, L"component") == 0) {
                if (!current) continue;
                Component comp;
                bool hasId = false;
                bool badPath = false;
                ForEachAttribute(reader.Get(), [&](const wchar_t* attr, const wchar_t* value, UINT length) {
                    if (std::wcscmp(attr, L"objectid") == 0) {
                        hasId = ParseUint32(value, length, comp.objectId);
                    } else if (std::wcscmp(attr, L"path") == 0) {
                        if (length > kMaxPathLength) badPath = true;
                        else comp.path = NormalizePartName(std::wstring(value, length));
                    } else if (std::wcscmp(attr, L"transform") == 0) {
                        comp.transform = ParseTransform(value, length);
                    }
                });
                if (hasId && !badPath) {
                    if (!CountNode()) return false;
                    current->comps.push_back(std::move(comp));
                }
            } else if (std::wcscmp(name, L"item") == 0) {
                if (!inBuild) continue;
                BuildItem item;
                bool hasId = false;
                bool printable = true;
                bool badPath = false;
                ForEachAttribute(reader.Get(), [&](const wchar_t* attr, const wchar_t* value, UINT length) {
                    if (std::wcscmp(attr, L"objectid") == 0) {
                        hasId = ParseUint32(value, length, item.objectId);
                    } else if (std::wcscmp(attr, L"path") == 0) {
                        if (length > kMaxPathLength) badPath = true;
                        else item.path = NormalizePartName(std::wstring(value, length));
                    } else if (std::wcscmp(attr, L"transform") == 0) {
                        item.transform = ParseTransform(value, length);
                    } else if (std::wcscmp(attr, L"printable") == 0) {
                        printable = !(length == 1 && value[0] == L'0');
                    }
                });
                if (hasId && printable && !badPath) {
                    if (!CountNode()) return false;
                    doc.items.push_back(std::move(item));
                }
            } else if (std::wcscmp(name, L"build") == 0) {
                inBuild = !empty;
            } else if (std::wcscmp(name, L"model") == 0) {
                ForEachAttribute(reader.Get(), [&](const wchar_t* attr, const wchar_t* value, UINT length) {
                    if (std::wcscmp(attr, L"unit") != 0) return;
                    const std::wstring unit(value, length);
                    if (unit == L"micron") doc.unitScale = 0.001;
                    else if (unit == L"millimeter") doc.unitScale = 1.0;
                    else if (unit == L"centimeter") doc.unitScale = 10.0;
                    else if (unit == L"inch") doc.unitScale = 25.4;
                    else if (unit == L"foot") doc.unitScale = 304.8;
                    else if (unit == L"meter") doc.unitScale = 1000.0;
                });
            }
        }
        return true;
    }

    // Counts a stored <object>/<component>/<item> element against the package budget.
    bool CountNode() {
        if (++m_totalNodes > kMaxStoredNodes) {
            m_error = L"3MF: model çok büyük";
            return false;
        }
        return true;
    }

    template <typename Fn>
    static void ForEachAttribute(IXmlReader* reader, Fn&& fn) {
        HRESULT hr = reader->MoveToFirstAttribute();
        while (hr == S_OK) {
            const wchar_t* attr = nullptr;
            const wchar_t* value = nullptr;
            UINT length = 0;
            if (SUCCEEDED(reader->GetLocalName(&attr, nullptr)) && attr &&
                SUCCEEDED(reader->GetValue(&value, &length)) && value) {
                fn(attr, value, length);
            }
            hr = reader->MoveToNextAttribute();
        }
        reader->MoveToElement();
    }

    // Adds an upper bound of the triangles Emit will produce (including invalid indices) to `total`. Same
    // tree, depth and call limits as Emit; error if kMaxOutputTriangles is exceeded.
    bool Count(ModelDoc& doc, uint32_t objectId, int depth, uint64_t& total) {
        if (depth > kMaxComponentDepth) {
            m_error = L"3MF: bileşen derinliği sınırı aşıldı";
            return false;
        }
        if (++m_countCalls > kMaxEmitCalls) {
            m_error = L"3MF: model çok büyük";
            return false;
        }
        if ((m_countCalls & 0xFFF) == 0 && m_deadline.Expired()) return TimedOut();
        auto it = doc.objects.find(objectId);
        if (it == doc.objects.end()) return true;
        const Object& object = it->second;
        total += object.tris.size() / 3;
        if (total > kMaxOutputTriangles) {
            m_error = L"3MF: model çok büyük";
            return false;
        }
        for (const Component& comp : object.comps) {
            ModelDoc* child = &doc;
            if (!comp.path.empty()) {
                child = GetDoc(comp.path);
                if (!m_error.empty()) return false;
                if (!child) continue;
            }
            if (!Count(*child, comp.objectId, depth + 1, total)) return false;
        }
        return true;
    }

    bool Emit(ModelDoc& doc, uint32_t objectId, const Transform& transform, int depth) {
        if (depth > kMaxComponentDepth) {
            m_error = L"3MF: bileşen derinliği sınırı aşıldı";
            return false;
        }
        if (++m_emitCalls > kMaxEmitCalls) {
            m_error = L"3MF: model çok büyük";
            return false;
        }
        if ((m_emitCalls & 0xFFF) == 0 && m_deadline.Expired()) return TimedOut();
        auto it = doc.objects.find(objectId);
        if (it == doc.objects.end()) return true;  // missing object: tolerated
        const Object& object = it->second;

        const size_t vertexCount = object.verts.size() / 3;
        const size_t triangleCount = object.tris.size() / 3;
        if (triangleCount > 0) {
            // Work budget: triangles visited (triangles with invalid indices take time even without output).
            m_visitedTriangles += triangleCount;
            if (m_visitedTriangles > kMaxOutputTriangles ||
                m_raw.size() / 9 + triangleCount > kMaxOutputTriangles) {
                m_error = L"3MF: model çok büyük";
                return false;
            }
            const bool mirrored = transform.Determinant() < 0;
            const double scale = m_rootScale;
            // Count() allocated the raw array at full size; this is only a safety net (no geometric growth).
            const size_t needed = m_raw.size() + triangleCount * 9;
            if (m_raw.capacity() < needed) m_raw.reserve(needed);
            for (size_t t = 0; t < triangleCount; ++t) {
                if ((t & 0xFFFF) == 0xFFFF && m_deadline.Expired()) return TimedOut();
                const uint32_t* idx = &object.tris[t * 3];
                if (idx[0] >= vertexCount || idx[1] >= vertexCount || idx[2] >= vertexCount) continue;
                uint32_t order[3] = {idx[0], idx[1], idx[2]};
                if (mirrored) std::swap(order[1], order[2]);
                for (uint32_t vi : order) {
                    const float* p = &object.verts[static_cast<size_t>(vi) * 3];
                    double x, y, z;
                    transform.Apply(p[0], p[1], p[2], x, y, z);
                    m_raw.push_back(ToFloat(x * scale));
                    m_raw.push_back(ToFloat(y * scale));
                    m_raw.push_back(ToFloat(z * scale));
                }
            }
        }

        for (const Component& comp : object.comps) {
            ModelDoc* child = &doc;
            if (!comp.path.empty()) {
                child = GetDoc(comp.path);
                if (!m_error.empty()) return false;
                if (!child) continue;
            }
            if (!Emit(*child, comp.objectId, Multiply(comp.transform, transform), depth + 1)) return false;
        }
        return true;
    }

    std::vector<float>& m_raw;
    std::wstring& m_error;
    const LoadDeadline& m_deadline;
    ComPtr<IOpcFactory> m_factory;
    ComPtr<IOpcPackage> m_package;
    std::map<std::wstring, ComPtr<IOpcPart>> m_parts;
    std::vector<std::wstring> m_partOrder;
    std::map<std::wstring, ModelDoc> m_docs;
    std::unordered_set<std::wstring> m_missing;
    double m_rootScale = 1.0;
    uint64_t m_totalVertices = 0;
    uint64_t m_totalTriangles = 0;
    uint64_t m_emitCalls = 0;
    uint64_t m_countCalls = 0;
    uint64_t m_visitedTriangles = 0;
    uint64_t m_totalNodes = 0;
    ReadBudget m_budget;
};

// Initializes COM temporarily if it is not initialized on this thread; undone in the destructor.
class ComScope {
public:
    ComScope() {
        // Only to probe the state: CoGetApartmentType returns CO_E_NOTINITIALIZED if not initialized.
        APTTYPE type;
        APTTYPEQUALIFIER qualifier;
        if (CoGetApartmentType(&type, &qualifier) == CO_E_NOTINITIALIZED) {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            m_owned = SUCCEEDED(hr);
        }
    }
    ~ComScope() {
        if (m_owned) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool m_owned = false;
};

}  // namespace

bool Parse3mfTriangles(const uint8_t* data, size_t size, std::vector<float>& raw, uint32_t& objectCount,
                       std::wstring& error, const LoadDeadline& deadline) {
    raw.clear();
    objectCount = 0;
    error.clear();
    if (!data || size < 22) {  // the smallest zip (end-of-directory record only) is 22 bytes
        error = L"Dosya boş veya çok küçük";
        return false;
    }
    if (size > 0xFFFFFFFFull) {
        error = L"3MF: model çok büyük";
        return false;
    }

    bool ok = false;
    {
        ComScope com;  // constructed BEFORE the ComPtrs, destroyed AFTER
        ThreeMfReader reader(raw, error, deadline);
        ok = reader.Read(data, size, objectCount);
    }
    if (!ok) {
        std::vector<float>().swap(raw);
        objectCount = 0;
        if (error.empty()) error = L"3MF paketi açılamadı (0x80004005)";
    }
    return ok;
}
