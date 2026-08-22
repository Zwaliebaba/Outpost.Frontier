#include "pch.h"
#include "CppUnitTest.h"

#include "ObjMesh.h"

#include <DirectXMath.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The OBJ/MTL loader (Build Order S5).
 *
 * Two halves. The first feeds hand-written text to the parser, so the expected
 * counts and ranges are things a person worked out rather than things the code
 * once produced. The second loads the nine meshes actually in GameData and
 * checks their real counts -- which is the half that catches a mesh being
 * re-exported with a sixth material or a stray quad.
 *
 * No device anywhere: this is text and arithmetic.
 */

namespace NeuronClientTests
{

namespace
{

[[nodiscard]] bool IsDirectory(const std::string& _path)
{
  const DWORD attributes = GetFileAttributesA(_path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// This DLL's own directory. The working directory a test host runs with is not
/// contractual -- it has been the test assembly's folder, the solution root and
/// a temporary results folder across versions -- so the search starts from
/// something that is known: where this binary actually is.
std::string TestBinaryDirectory()
{
  HMODULE module = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(&IsDirectory), &module) == 0)
  {
    return {};
  }

  char path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH)
  {
    return {};
  }

  std::string full(path, length);
  const std::size_t slash = full.find_last_of("\\/");
  return slash == std::string::npos ? std::string{} : full.substr(0, slash);
}

/// The content tree, found by walking up from the working directory and then
/// from this binary's own directory. Deliberately fails rather than skipping
/// when it cannot be found: a content test that quietly passes with no content
/// is worse than no test at all.
std::string FindMeshDirectory()
{
  const std::string roots[] = {std::string{}, TestBinaryDirectory()};
  for (const std::string& root : roots)
  {
    std::string prefix = root.empty() ? std::string{} : root + "/";
    for (int depth = 0; depth < 8; ++depth)
    {
      const std::string candidate = prefix + "GameData/Meshes";
      if (IsDirectory(candidate))
      {
        return candidate;
      }
      prefix += "../";
    }
  }
  Assert::Fail(L"GameData/Meshes was not found from the working directory or from beside the test binary");
  return {};
}

/// A minimal but complete mesh: one triangle per material, in a deliberately
/// scrambled order so the material grouping has something to do.
constexpr const char* SCRAMBLED_MESH = "mtllib Test.mtl\n"
                                       "o test\n"
                                       "v 0 0 0\n"
                                       "v 10 0 0\n"
                                       "v 0 0 10\n"
                                       "vn 0 1 0\n"
                                       "usemtl thruster\n"
                                       "f 1//1 2//1 3//1\n"
                                       "usemtl hull\n"
                                       "f 1//1 3//1 2//1\n"
                                       "usemtl thruster\n"
                                       "f 2//1 3//1 1//1\n"
                                       "usemtl accent\n"
                                       "f 3//1 1//1 2//1\n";

} // namespace

TEST_CLASS(ObjParserTests)
{
public:
  TEST_METHOD(GroupsFacesByMaterialWhateverOrderTheFileUses)
  {
    ObjMesh mesh;
    ObjDiagnostic error;
    Assert::IsTrue(ParseObjMesh(SCRAMBLED_MESH, mesh, error), L"the mesh should parse");

    Assert::AreEqual<std::size_t>(3, mesh.submeshes.size(), L"three materials are used, so there are three submeshes");
    Assert::AreEqual<std::uint32_t>(4, mesh.TriangleCount());

    // Canonical order, not file order: hull, plate, glass, accent, thruster.
    Assert::IsTrue(mesh.submeshes[0].material == MeshMaterial::Hull);
    Assert::IsTrue(mesh.submeshes[1].material == MeshMaterial::Accent);
    Assert::IsTrue(mesh.submeshes[2].material == MeshMaterial::Thruster);

    // The two thruster faces are one contiguous range despite the hull face
    // sitting between them in the file.
    Assert::AreEqual<std::uint32_t>(6, mesh.submeshes[2].indexCount);

    // Ranges partition the index buffer exactly: contiguous, whole triangles,
    // and covering every index.
    std::uint32_t expectedFirst = 0;
    for (const SubmeshRange& range : mesh.submeshes)
    {
      Assert::AreEqual(expectedFirst, range.firstIndex, L"submesh ranges must be contiguous");
      Assert::AreEqual<std::uint32_t>(0, range.indexCount % 3, L"a submesh is whole triangles");
      expectedFirst += range.indexCount;
    }
    Assert::AreEqual<std::size_t>(expectedFirst, mesh.indices.size(), L"the ranges must cover every index");

    Assert::AreEqual(std::string("Test.mtl"), mesh.materialLibrary);
  }

  TEST_METHOD(SharesVerticesWithinAMaterialAndAcrossThem)
  {
    ObjMesh mesh;
    ObjDiagnostic error;
    Assert::IsTrue(ParseObjMesh(SCRAMBLED_MESH, mesh, error));

    // Four triangles over three positions and one normal: every corner names
    // the same (position, normal) pairs, so there are exactly three vertices.
    Assert::AreEqual<std::size_t>(3, mesh.vertices.size());
    for (std::uint32_t index : mesh.indices)
    {
      Assert::IsTrue(index < mesh.vertices.size(), L"every index must be in range");
    }
  }

  TEST_METHOD(SplitsVerticesWhereTheNormalDiffers)
  {
    // Same three positions, two different normals: a hard edge, and a hard edge
    // is what makes flat shading flat.
    constexpr const char* text = "usemtl hull\n"
                                 "v 0 0 0\nv 1 0 0\nv 0 0 1\n"
                                 "vn 0 1 0\nvn 1 0 0\n"
                                 "f 1//1 2//1 3//1\n"
                                 "f 1//2 2//2 3//2\n";
    ObjMesh mesh;
    ObjDiagnostic error;
    Assert::IsTrue(ParseObjMesh(text, mesh, error));
    Assert::AreEqual<std::size_t>(6, mesh.vertices.size(), L"three positions x two normals is six vertices");
  }

  TEST_METHOD(AcceptsRelativeIndicesAndFansPolygons)
  {
    constexpr const char* text = "usemtl plate\n"
                                 "v 0 0 0\nv 1 0 0\nv 1 0 1\nv 0 0 1\n"
                                 "vn 0 1 0\n"
                                 "f -4//-1 -3//-1 -2//-1 -1//-1\n";
    ObjMesh mesh;
    ObjDiagnostic error;
    Assert::IsTrue(ParseObjMesh(text, mesh, error), L"negative indices are legal OBJ");
    Assert::AreEqual<std::uint32_t>(2, mesh.TriangleCount(), L"a quad fans into two triangles");
  }

  TEST_METHOD(DerivesANormalWhenTheFileOmitsOne)
  {
    constexpr const char* text = "usemtl hull\nv 0 0 0\nv 10 0 0\nv 0 0 10\nf 1 2 3\n";
    ObjMesh mesh;
    ObjDiagnostic error;
    Assert::IsTrue(ParseObjMesh(text, mesh, error));
    Assert::AreEqual<std::uint32_t>(1, mesh.TriangleCount());

    // The triangle lies in the y = 0 plane, so its normal is vertical.
    const float length = std::abs(mesh.vertices[0].normal.y);
    Assert::AreEqual(1.0f, length, 1e-4f, L"a derived normal is a unit face normal");
  }

  TEST_METHOD(ComputesBoundsAndRadius)
  {
    constexpr const char* text = "usemtl hull\nv -3 0 0\nv 0 4 0\nv 0 0 0\nvn 0 1 0\nf 1//1 2//1 3//1\n";
    ObjMesh mesh;
    ObjDiagnostic error;
    Assert::IsTrue(ParseObjMesh(text, mesh, error));

    Assert::AreEqual(-3.0f, mesh.boundsMin.x, 1e-5f);
    Assert::AreEqual(4.0f, mesh.boundsMax.y, 1e-5f);
    Assert::AreEqual(4.0f, mesh.radiusMetres, 1e-4f, L"the radius is the furthest vertex from the origin");
  }

  TEST_METHOD(ThePlaneRadiusIsTheFootprintAndNotTheSphere)
  {
    /*
     * A ring standing up in Y reads as its footprint from a fixed-elevation
     * camera, not as its height -- so the silhouette is measured on the plane.
     * Measured in three dimensions the mesh below would answer 5, which is a
     * number about how tall it is.
     */
    constexpr const char* text = "usemtl hull\nv 3 4 0\nv 0 4 0\nv 0 0 0\nvn 0 1 0\nf 1//1 2//1 3//1\n";
    ObjMesh mesh;
    ObjDiagnostic error;
    Assert::IsTrue(ParseObjMesh(text, mesh, error));

    Assert::AreEqual(3.0f, PlaneRadiusMetres(mesh), 1e-4f, L"hypot(x, z), never y");
    Assert::AreEqual(5.0f, mesh.radiusMetres, 1e-4f, L"the sphere still answers about the sphere");
  }

  TEST_METHOD(FittingScalesEveryVertexAndRecomputesWhatDependsOnThem)
  {
    constexpr const char* text = "usemtl hull\nv 3 4 0\nv 0 4 0\nv 0 0 0\nvn 0 1 0\nf 1//1 2//1 3//1\n";
    ObjMesh mesh;
    ObjDiagnostic error;
    Assert::IsTrue(ParseObjMesh(text, mesh, error));

    Assert::IsTrue(FitObjMeshToPlaneRadius(mesh, 12.0f));
    Assert::AreEqual(12.0f, PlaneRadiusMetres(mesh), 1e-4f, L"the target is met exactly");

    // Uniform, so height goes with width: a hull that grew wider and stayed
    // flat would read as a decal rather than a ship.
    Assert::AreEqual(16.0f, mesh.boundsMax.y, 1e-3f, L"y scaled by the same factor");
    Assert::AreEqual(20.0f, mesh.radiusMetres, 1e-3f, L"and the sphere was recomputed, not left stale");

    // The normal is untouched and still unit: a uniform scale does not rotate
    // or stretch a direction, so normalising again would be work to arrive
    // back where it started.
    const DirectX::XMFLOAT3& normal = mesh.vertices[0].normal;
    const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    Assert::AreEqual(1.0f, length, 1e-5f);
  }

  TEST_METHOD(NoOpinionLeavesTheArtExactlyAsAuthored)
  {
    /*
     * Zero has to mean "as authored" rather than "scale to nothing", because it
     * is what a caller with no answer passes -- a mesh the game has no hull
     * for, or a shorter list than the file list. The failure this pins is a
     * fleet that loads and draws at the origin as points.
     */
    constexpr const char* text = "usemtl hull\nv 3 4 0\nv 0 4 0\nv 0 0 0\nvn 0 1 0\nf 1//1 2//1 3//1\n";
    for (const float target : {0.0f, -1.0f})
    {
      ObjMesh mesh;
      ObjDiagnostic error;
      Assert::IsTrue(ParseObjMesh(text, mesh, error));
      Assert::IsFalse(FitObjMeshToPlaneRadius(mesh, target), L"nothing asked, nothing done");
      Assert::AreEqual(3.0f, PlaneRadiusMetres(mesh), 1e-4f);
      Assert::AreEqual(5.0f, mesh.radiusMetres, 1e-4f);
    }

    // And a mesh with no width on the plane cannot be given one. Dividing by
    // its zero radius would take the whole fleet with it.
    constexpr const char* upright = "usemtl hull\nv 0 4 0\nv 0 2 0\nv 0 0 0\nvn 0 1 0\nf 1//1 2//1 3//1\n";
    ObjMesh flat;
    ObjDiagnostic error;
    Assert::IsTrue(ParseObjMesh(upright, flat, error));
    Assert::IsFalse(FitObjMeshToPlaneRadius(flat, 12.0f));
    Assert::AreEqual(4.0f, flat.radiusMetres, 1e-4f, L"left exactly as authored");
  }

  TEST_METHOD(MalformedContentIsADiagnosticWithALineAndAColumn)
  {
    struct Case
    {
      const char* text;
      std::uint32_t line;
      const wchar_t* what;
    };

    // Every one of these is a person's mistake in an authored file, so every
    // one has to come back as a location rather than as a crash (AGENTS.md §5).
    const Case cases[] = {
        {"usemtl chrome\n", 1, L"an unknown material"},
        {"usemtl hull\nv 0 zero 0\n", 2, L"a number that is not a number"},
        {"usemtl hull\nv 0 0\n", 2, L"a vertex short of a component"},
        {"usemtl hull\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 9\n", 5, L"an index past the end"},
        {"usemtl hull\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2\n", 5, L"a face with two corners"},
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", 4, L"a face before any usemtl"},
        {"usemtl hull\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/1/1/1 2 3\n", 5, L"a corner with four fields"},
        {"# a file with no faces in it\n", 2, L"a file that is not a mesh"},
    };

    for (const Case& testCase : cases)
    {
      ObjMesh mesh;
      ObjDiagnostic error;
      Assert::IsFalse(ParseObjMesh(testCase.text, mesh, error), testCase.what);
      Assert::AreEqual(testCase.line, error.line, testCase.what);
      Assert::IsTrue(error.column >= 1, L"a diagnostic carries a 1-based column");
      Assert::IsFalse(error.message.empty(), L"a diagnostic says what is wrong");
    }
  }

  TEST_METHOD(ParsesTheCanonicalMaterialPalette)
  {
    constexpr const char* text = "# Exported by three-d-stage\n"
                                 "newmtl hull\nKd 0.25 0.5 0.75\nKs 0.2 0.2 0.2\nNs 44\nd 1.0\n"
                                 "newmtl thruster\nKd 1.0 0.5 0.0\n";
    MeshMaterialPalette palette;
    ObjDiagnostic error;
    Assert::IsTrue(ParseMeshMaterials(text, palette, error));

    Assert::IsTrue(palette.defined[static_cast<std::uint32_t>(MeshMaterial::Hull)]);
    Assert::AreEqual(0.5f, palette.albedo[static_cast<std::uint32_t>(MeshMaterial::Hull)].y, 1e-5f);
    Assert::IsTrue(palette.defined[static_cast<std::uint32_t>(MeshMaterial::Thruster)]);
    Assert::IsFalse(palette.defined[static_cast<std::uint32_t>(MeshMaterial::Glass)], L"an unmentioned material stays undefined");

    MeshMaterialPalette rejected;
    ObjDiagnostic rejectedError;
    Assert::IsFalse(ParseMeshMaterials("newmtl chrome\nKd 1 1 1\n", rejected, rejectedError));
    Assert::AreEqual<std::uint32_t>(1, rejectedError.line);
  }

  TEST_METHOD(MaterialNamesAreAClosedSet)
  {
    MeshMaterial material = MeshMaterial::Glass;
    Assert::IsTrue(ParseMeshMaterialName("hull", material));
    Assert::IsTrue(material == MeshMaterial::Hull);
    Assert::IsFalse(ParseMeshMaterialName("Hull", material), L"the corpus authors lower case, and matching is exact");
    Assert::IsFalse(ParseMeshMaterialName("", material));
    Assert::AreEqual("thruster", MeshMaterialName(MeshMaterial::Thruster));
  }

  TEST_METHOD(AMissingFileIsADiagnosticNotAnException)
  {
    ObjMesh mesh;
    ObjDiagnostic error;
    Assert::IsFalse(LoadObjMesh(FindMeshDirectory(), "NoSuchShip.obj", mesh, error));
    Assert::AreEqual(std::string("NoSuchShip.obj"), error.file);
    Assert::IsFalse(error.message.empty());
  }
};

TEST_CLASS(ShippedMeshTests)
{
public:
  TEST_METHOD(TheNineMeshesLoadWithTheCountsTheyWereAuthoredWith)
  {
    // Counted from the files themselves: `grep -c "^f " <mesh>.obj` is the
    // triangle count, and the vertex count is the number of `v` lines that any
    // face actually references. If a mesh is re-exported, this test is the
    // thing that says so.
    struct Expected
    {
      const char* file;
      std::uint32_t vertices;
      std::uint32_t triangles;
      std::size_t submeshes;
    };

    const Expected meshes[] = {
        {"Interceptor.obj", 780, 312, 5}, {"Bomber.obj", 796, 376, 5},     {"Corvette.obj", 716, 324, 5},
        {"Frigate.obj", 948, 464, 4},     {"Hauler.obj", 532, 268, 5},     {"Miner.obj", 1162, 508, 5},
        {"Carrier.obj", 1208, 576, 5},    {"Battleship.obj", 1344, 664, 5}, {"Structure.obj", 3704, 1784, 5},
        {"Stargate.obj", 1888, 1144, 5},
    };

    const std::string directory = FindMeshDirectory();
    for (const Expected& expected : meshes)
    {
      ObjMesh mesh;
      ObjDiagnostic error;
      const std::wstring name(std::wstring(expected.file, expected.file + std::strlen(expected.file)));
      Assert::IsTrue(LoadObjMesh(directory, expected.file, mesh, error), name.c_str());

      Assert::AreEqual<std::size_t>(expected.vertices, mesh.vertices.size(), name.c_str());
      Assert::AreEqual(expected.triangles, mesh.TriangleCount(), name.c_str());
      Assert::AreEqual(expected.submeshes, mesh.submeshes.size(), name.c_str());

      std::uint32_t expectedFirst = 0;
      for (const SubmeshRange& range : mesh.submeshes)
      {
        Assert::AreEqual(expectedFirst, range.firstIndex, name.c_str());
        expectedFirst += range.indexCount;
      }
      Assert::AreEqual<std::size_t>(expectedFirst, mesh.indices.size(), name.c_str());

      for (std::uint32_t index : mesh.indices)
      {
        Assert::IsTrue(index < mesh.vertices.size(), name.c_str());
      }

      Assert::IsTrue(mesh.radiusMetres > 0.0f, name.c_str());
      Assert::IsTrue(mesh.palette.defined[static_cast<std::uint32_t>(MeshMaterial::Hull)], name.c_str());
    }
  }

  TEST_METHOD(ADerivedNormalPointsTheSameWayAsAnAuthoredOne)
  {
    // The missing-normal fallback takes a cross product, which is the one place
    // in the loader that depends on the tree being left-handed (ADR-006 §3a).
    // Nothing in the shipped corpus exercises it -- every mesh authors its
    // normals -- so if the operands were ever swapped, no mesh we ship would
    // say so and the first normal-less OBJ anyone hand-edits would light
    // inside out. Recomputing the corpus's own normals from its own winding is
    // what keeps the fallback honest.
    //
    // The assertion is on the *sign*, and only the sign, because that is what
    // handedness decides. It is deliberately not "the derived normal equals the
    // authored one": a minority of the corpus is smooth-shaded (152 of
    // Structure's 1,784 faces carry a different normal per corner), so on those
    // faces the geometric normal legitimately differs from the authored one by
    // tens of degrees. Measured across the ten meshes the worst alignment is
    // about +0.68 -- the stargate's, which is the tightest in the corpus and
    // still nowhere near the bound -- and none is negative; swapping the cross
    // operands would make all 6,420 of them negative at once.
    const std::string directory = FindMeshDirectory();
    const char* files[] = {"Interceptor.obj", "Bomber.obj",  "Corvette.obj",   "Frigate.obj",  "Hauler.obj",
                           "Miner.obj",       "Carrier.obj", "Battleship.obj", "Structure.obj", "Stargate.obj"};

    std::uint32_t compared = 0;
    for (const char* file : files)
    {
      ObjMesh mesh;
      ObjDiagnostic error;
      Assert::IsTrue(LoadObjMesh(directory, file, mesh, error));

      for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3)
      {
        const MeshVertex& a = mesh.vertices[mesh.indices[triangle]];
        const MeshVertex& b = mesh.vertices[mesh.indices[triangle + 1]];
        const MeshVertex& c = mesh.vertices[mesh.indices[triangle + 2]];

        const DirectX::XMVECTOR edge0 = DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&b.position),
                                                                  DirectX::XMLoadFloat3(&a.position));
        const DirectX::XMVECTOR edge1 = DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&c.position),
                                                                  DirectX::XMLoadFloat3(&a.position));
        DirectX::XMFLOAT3 derived;
        DirectX::XMStoreFloat3(&derived, DirectX::XMVector3Normalize(DirectX::XMVector3Cross(edge0, edge1)));

        const float alignment = derived.x * a.normal.x + derived.y * a.normal.y + derived.z * a.normal.z;
        Assert::IsTrue(alignment > 0.1f, L"a normal derived from the winding must not point against the authored one");
        ++compared;
      }
    }

    // 6,420 triangles across the ten meshes -- 5,276 before U4's stargate. The
    // bound is a floor rather than the exact count so a re-export does not fail
    // here instead of failing the count test above, but it is high enough that
    // a silently empty loop -- which would make every assertion above vacuous
    // -- cannot pass.
    Assert::IsTrue(compared > 5000, L"the comparison must actually have run over the corpus");
  }

  TEST_METHOD(TheCorpusAuthorsTheCanonicalPaletteAndNotSomeOtherGreen)
  {
    /*
     * The palette the art direction names, to the digit the `.mtl` files carry.
     *
     * `OnlyFrigateLacksGlassAndTheRestSharePalettes` proves the ten files agree
     * with *each other*, which is a different and weaker claim: a re-export
     * that recoloured every hull to grey would pass it unanimously. This one
     * pins what the colours actually are.
     *
     * The numbers are **linear**, which is the part worth stating rather than
     * discovering. The render target is `_SRGB`, so a Kd reaches the screen
     * unconverted and the files author the linear floats directly -- 0.0203
     * 0.0331 0.0242 is the #27332b of the design table, not a second opinion
     * about it. Anyone "correcting" these to sRGB fails here, which is the
     * point of writing them down twice.
     */
    struct Expected
    {
      MeshMaterial material;
      float red;
      float green;
      float blue;
      const wchar_t* designHex;
    };

    const Expected palette[] = {
        {MeshMaterial::Hull, 0.0203f, 0.0331f, 0.0242f, L"hull #27332b"},
        {MeshMaterial::Plate, 0.1070f, 0.1470f, 0.0908f, L"plate #5c6b55"},
        {MeshMaterial::Glass, 0.0044f, 0.0116f, 0.0080f, L"glass #0e1c16"},
        {MeshMaterial::Accent, 0.3278f, 0.8632f, 0.0130f, L"accent #9bef1e"},
        {MeshMaterial::Thruster, 0.5029f, 0.9301f, 0.1301f, L"thruster #bcf765"},
    };

    const std::string directory = FindMeshDirectory();
    const char* files[] = {"Interceptor.obj", "Bomber.obj",  "Corvette.obj",   "Frigate.obj",   "Hauler.obj",
                           "Miner.obj",       "Carrier.obj", "Battleship.obj", "Structure.obj", "Stargate.obj"};

    std::uint32_t checked = 0;
    for (const char* file : files)
    {
      ObjMesh mesh;
      ObjDiagnostic error;
      Assert::IsTrue(LoadObjMesh(directory, file, mesh, error));

      for (const Expected& expected : palette)
      {
        const auto index = static_cast<std::uint32_t>(expected.material);
        if (!mesh.palette.defined[index])
        {
          continue; // Frigate authors no glass, and a mesh need not use all five.
        }
        Assert::AreEqual(expected.red, mesh.palette.albedo[index].x, 1e-6f, expected.designHex);
        Assert::AreEqual(expected.green, mesh.palette.albedo[index].y, 1e-6f, expected.designHex);
        Assert::AreEqual(expected.blue, mesh.palette.albedo[index].z, 1e-6f, expected.designHex);
        ++checked;
      }
    }

    // Ten files times five materials, less the one glass the Frigate does not
    // author. A floor rather than the exact count, so a mesh added to the
    // corpus does not fail here instead of failing the count test -- but high
    // enough that a loop that silently found nothing cannot pass.
    Assert::IsTrue(checked >= 49, L"every material every mesh authors must have been compared");
  }

  TEST_METHOD(OnlyFrigateLacksGlassAndTheRestSharePalettes)
  {
    // The corpus authors one palette across ten files; a mesh that drifts is a
    // content bug the renderer would show as a recolour and say nothing about.
    //
    // `Stargate.obj` is the newest of them and the one that proves the check is
    // worth running: it arrived carrying a sixth material, `aperture`, whose
    // albedo was the accent colour to the last digit and whose only difference
    // was a `d 0.2` this renderer does not read (ADR-006 §5 is albedo plus a
    // light term). The palette is five, so the two ring faces that used it were
    // authored onto `accent` instead -- the same pixels, and a mesh the loader
    // accepts rather than one it refuses at boot.
    const std::string directory = FindMeshDirectory();
    const char* files[] = {"Interceptor.obj", "Bomber.obj",  "Corvette.obj",   "Frigate.obj",  "Hauler.obj",
                           "Miner.obj",       "Carrier.obj", "Battleship.obj", "Structure.obj", "Stargate.obj"};

    ObjMesh reference;
    ObjDiagnostic error;
    Assert::IsTrue(LoadObjMesh(directory, "Battleship.obj", reference, error));

    for (const char* file : files)
    {
      ObjMesh mesh;
      Assert::IsTrue(LoadObjMesh(directory, file, mesh, error));
      for (std::uint32_t material = 0; material < MESH_MATERIAL_COUNT; ++material)
      {
        if (!mesh.palette.defined[material])
        {
          continue;
        }
        Assert::AreEqual(reference.palette.albedo[material].x, mesh.palette.albedo[material].x, 1e-6f);
        Assert::AreEqual(reference.palette.albedo[material].y, mesh.palette.albedo[material].y, 1e-6f);
        Assert::AreEqual(reference.palette.albedo[material].z, mesh.palette.albedo[material].z, 1e-6f);
      }
    }
  }
};

} // namespace NeuronClientTests
