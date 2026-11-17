#pragma once

// model.h — shared model (mesh) contract. Every unit only READS this file;
// it is owned by the "mesh" unit (implementation: model_loader.cpp, model_3mf.cpp, stl_mesh.cpp).
//
// COORDINATE CONTRACT (binding for everyone)
// -------------------------------------------
// * Units are millimetres everywhere. File axes are kept: X right, Y back, Z UP (right-handed).
// * "Original space": coordinates as in the file (for 3MF, with transforms and unit conversion applied).
// * "Scene space"   : coordinates in Mesh::vertices. Derived from original space by translation only:
//       scene = original - sceneOffset
//   sceneOffset = { (minX+maxX)/2, (minY+maxY)/2, minZ }.
//   So the model is centred on the bed in XY and RESTS ON THE BED: min Z in the scene = 0.
//   The bed plane is z = 0 in the scene. There is NO rotation / scaling / axis swap.
// * bounds (scene space) = { -sx/2, -sy/2, 0,  +sx/2, +sy/2, sz }.
// * Orbit (camera) centre = centre of bounds = (0, 0, sz/2).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class ModelFormat : uint32_t {
    Unknown = 0,
    Stl = 1,
    ThreeMf = 2,
};

// LoadModel flags (bit field).
enum ModelLoadFlags : uint32_t {
    kModelLoadDefault = 0,
    // Skip open edge COUNTING -> Mesh::openEdges = -1. Used by thumbnails.
    // (Together with kModelLoadSkipFeatureEdges, vertex welding is skipped entirely.)
    kModelLoadSkipTopology = 1u << 0,
    // Do not collect feature edges (Mesh::featureEdges) -> featureEdgeStatus = Skipped.
    kModelLoadSkipFeatureEdges = 1u << 1,
};

// State of Mesh::featureEdges.
enum class FeatureEdgeStatus : uint8_t {
    Skipped = 0,   // not requested, mesh too large (> kTopologyMaxTriangles), out of memory or time
    Ok = 1,        // featureEdges complete (may be empty: no sharp edges)
    TooMany = 2,   // kFeatureEdgeMaxSegments exceeded; featureEdges left empty
};

// Constants (single source; do not redefine elsewhere).
// Typical print mass estimate (MeshTypicalPrintMassGrams). No filament is named; the goal is the mass of the part
// an ordinary user would print with the slicer's (Cura / PrusaSlicer / Orca / Creality Print) DEFAULT settings:
//   * density: average of common filaments (PLA ~1.24, PETG ~1.27 g/cm³) -> 1.24;
//   * default profile: 0.2 mm layers, 2 walls (~0.85 mm), ~4 bottom/top layers (~0.8 mm), 15% infill.
// Model (local thickness): area-weighted samples are taken on the surface; from each sample a ray is cast inward
//        (-normal) to find the distance d to the opposite surface; sample shell depth = min(t_face, d / 2), with
//        t_face = kTypicalWallMm (near-vertical face) or kTypicalTopBottomMm (|nz| > 0.7).
//        shell = min(volume, Σ area × depth) (Mesh::printShellMm3). On thin walls the shells of the two
//        faces are not counted twice.
//        Fallback (thickness not computed): shell = min(volume, surface area × kTypicalShellMm).
//        printed volume = shell + kTypicalInfill × (volume - shell), mass = printed volume × kTypicalFilamentDensity.
// Supports, brim and purge are NOT included. For thin-walled parts the result approaches the solid mass.
// Average of the defaults: Cura 2 walls / 0.8 mm bottom-top / 20%, PrusaSlicer 2 perimeters / 15%,
// Orca / Bambu / Creality Print 2 walls / 15%.
inline constexpr double kTypicalFilamentDensity = 1.24;          // g/cm³
inline constexpr double kTypicalShellMm = 0.8;                   // fallback formula: average shell thickness, mm
inline constexpr double kTypicalWallMm = 0.84;                   // 2 walls × 0.42 mm (0.4 nozzle)
inline constexpr double kTypicalTopBottomMm = 0.8;               // average of bottom ~0.6 / top ~1.0 mm
inline constexpr double kTypicalInfill = 0.20;                   // infill ratio (0..1); owner decision 2026-11-17: 20%
inline constexpr uint64_t kTopologyMaxTriangles = 3'000'000;     // above this, no open / feature edges
inline constexpr double kWeldQuantumMm = 1e-4;                   // vertex weld grid
inline constexpr float kBedContactEpsMm = 0.05f;                 // tolerance for faces touching the bed (overhang)
// Overhang face: normal.z < -kOverhangCos. 1° of margin so that exact 45° faces (normal.z = -0.7071) do not
// stripe orange/grey from floating-point noise: a face counts as overhang if it leans more than 46° from vertical
// (angle between its normal and straight down below 44°). The UI text "steeper than 45°" stays correct.
inline constexpr float kOverhangCos = 0.71933980f;               // cos(44°) = sin(46°)
inline constexpr double kFeatureEdgeCos = 0.86602540378;         // cos(30°): if adjacent faces diverge more,
                                                                 // the edge counts as "sharp"
inline constexpr size_t kFeatureEdgeMaxSegments = 1'500'000;     // above this, feature edges are not kept
// LoadModel time budgets (ms, loadBudgetMs). If exceeded, load fails with a timeout error; if only the edge
// analysis runs out of time, the model still loads (openEdges = -1, featureEdgeStatus = Skipped).
inline constexpr uint32_t kPreviewLoadBudgetMs = 8000;           // preview (prevhost)
inline constexpr uint32_t kThumbnailLoadBudgetMs = 3000;         // thumbnail (dllhost)

struct Mesh {
    // --- draw data (scene space) -------------------------------------------------------------
    // 6 floats per vertex: position(3) + face normal(3). Non-indexed, 3 vertices per triangle.
    // Normals are in model space (scene = model, translation only), unit length, constant per face.
    std::vector<float> vertices;
    // Triangle count in vertices (decimated if needed) = vertices.size() / 18.
    uint32_t triangleCount = 0;
    // Feature (sharp) edges, scene space: 6 floats per segment (ax ay az bx by bz).
    // Computed from the FULL triangle set (before decimation): edges whose adjacent face normals diverge > 30°,
    // boundary edges (one face) and non-manifold edges (> 2 faces). To reduce noise on scanned/organic meshes,
    // isolated sharp edge chains whose total length is under 1% of the bounding box diagonal are dropped
    // (open edges are always kept).
    // State: featureEdgeStatus.
    // The caller may free this after the renderer uploads it to the GPU (the preview frees it when opened from a
    // file path; when opened from a stream it keeps it to re-upload after device loss).
    std::vector<float> featureEdges;
    FeatureEdgeStatus featureEdgeStatus = FeatureEdgeStatus::Skipped;
    // Scene space bounding box: {minX, minY, minZ, maxX, maxY, maxZ} = {-sx/2, -sy/2, 0, sx/2, sy/2, sz}.
    float bounds[6]{};
    // Half diagonal of the bounding box (mm). Never 0 (1 when degenerate).
    float radius = 1.0f;

    // --- statistics: computed BEFORE DECIMATION, over the full triangle set -------------------
    ModelFormat format = ModelFormat::Unknown;
    // Number of valid (finite coordinates, non-degenerate) triangles in the file.
    uint64_t sourceTriangleCount = 0;
    // true if triangleCount < sourceTriangleCount (decimated because of the maxTriangles budget).
    bool decimated = false;
    // Exact bounding box in original space (mm).
    double origMin[3]{};
    double origMax[3]{};
    // Size (mm) = origMax - origMin. size[0]=X, size[1]=Y, size[2]=Z.
    double size[3]{};
    // original = scene + sceneOffset.
    double sceneOffset[3]{};
    // Absolute value of the signed tetrahedron sum, mm³. < 0 if not computed.
    // (Approximate if the mesh is not closed; the UI shows it with "≈" when openEdges > 0.)
    double volumeMm3 = -1.0;
    // Surface area, mm² (sum of triangle areas). < 0 if not computed.
    double surfaceAreaMm2 = -1.0;
    // Shell (wall + bottom/top) volume of a typical print, mm³, from local thickness (see note at kTypicalWallMm).
    // < 0: not computed (thumbnail, mesh too large, out of time) -> MeshTypicalPrintMassGrams uses the fallback.
    double printShellMm3 = -1.0;
    // Number of undirected edges whose use count is not 2. -1 = unknown (skipped, too large or out of time).
    int64_t openEdges = -1;
    // 3MF: number of merged build items. STL: 1.
    uint32_t objectCount = 1;
};

// ---------------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------------

// Detects the format from the extension (".stl", "a.3MF", "C:\\x\\y.stl" — only the part after the last '.'
// matters, case-insensitive) and/or from the content. With no/unknown hint it checks the content magic:
// starts with "PK\x03\x04" -> 3MF, otherwise STL. If data is null, only the hint is used.
// A ".3mf" hint always returns 3MF (if it is not a zip, LoadModel reports a "not a zip" error);
// with a ".stl" hint, content starting with "PK\x03\x04" counts as 3MF.
// LoadModel also rejects an STL that is neither size-consistent binary nor ASCII (read as binary as a last
// resort) if its bounding box exceeds 100 m on any axis (well-formed files have no such limit).
ModelFormat DetectModelFormat(const void* data, size_t size, const wchar_t* fileNameOrExtHint);

// Is this file name/extension a supported model (.stl / .3mf)?
bool IsSupportedModelExtension(const wchar_t* fileNameOrExt);

// Decodes an in-memory file. Returns true on success; on failure false, with a TURKISH message in `error`.
//   fileNameOrExtHint : may be null (file opened from a stream) -> content magic is used.
//   maxTriangles      : draw budget; 0 = unlimited. If exceeded, every Nth triangle is kept (stats are full).
//   flags             : ModelLoadFlags.
//   loadBudgetMs      : time budget (kPreviewLoadBudgetMs / kThumbnailLoadBudgetMs); 0 = unlimited.
// Thread safety: reentrant; no global state. For 3MF, COM should be initialized on the calling thread
// (if not, the loader initializes it temporarily itself).
// Does not throw (std::bad_alloc included; caught and turned into an error message).
bool LoadModel(const void* data, size_t size, const wchar_t* fileNameOrExtHint, uint32_t maxTriangles,
               Mesh& out, std::wstring& error, uint32_t flags = kModelLoadDefault,
               uint32_t loadBudgetMs = kPreviewLoadBudgetMs);

// ---------------------------------------------------------------------------------------------
// Formatting helpers (in model_loader.cpp; locale-INDEPENDENT). Separators depend on the UI language
// (i18n.h UiTurkish()): Turkish uses ',' decimal and '.' thousands, English '.' decimal and ',' thousands.
// The examples below use the Turkish format; the "Tr" in the names is historical.
// ---------------------------------------------------------------------------------------------

// 135.84 -> "135,8" (decimals = 1). Negative zero becomes "0,0". Does NOT use a thousands separator.
std::wstring FormatDecimalTr(double value, int decimals);

// 1234567 -> "1.234.567".
std::wstring FormatCountTr(uint64_t value);

// "X 135,8 × Y 134,8 × Z 30,0 mm"   (× = U+00D7)
std::wstring FormatSizeText(const Mesh& mesh);

// Clipboard text: "135,8 x 134,8 x 30,0 mm"   (plain ASCII 'x')
std::wstring FormatSizeClipboardText(const Mesh& mesh);

// Volume in cm³ (volumeMm3 / 1000). < 0 if unknown.
double MeshVolumeCm3(const Mesh& mesh);

// Surface area in cm² (surfaceAreaMm2 / 100). < 0 if unknown.
double MeshSurfaceAreaCm2(const Mesh& mesh);

// Fully solid mass, grams = cm³ * density. < 0 if the volume is unknown.
double MeshSolidMassGrams(const Mesh& mesh, double densityGcm3 = kTypicalFilamentDensity);

// Typical print mass with default slicer settings, grams (assumptions: see note at kTypicalFilamentDensity).
// < 0 if the volume is unknown; solid mass if the surface area is unknown.
double MeshTypicalPrintMassGrams(const Mesh& mesh);

// Mass estimate self-test (20 mm solid cube, 100×100×2 mm plate, 20 mm box with 1 mm walls). true on success;
// `report` holds the result for each case. Meant for the test tool.
bool ModelMassSelfTest(std::wstring& report);

// 3.456 -> "3,5 g" ; 38.07 -> "38 g" ; 1250 -> "1,25 kg"  (< 10 g 1 decimal, < 1000 g integer, then kg 2 decimals)
std::wstring FormatMassTr(double grams);

// Short format name: "STL" / "3MF" / "?".
const wchar_t* ModelFormatName(ModelFormat format);
