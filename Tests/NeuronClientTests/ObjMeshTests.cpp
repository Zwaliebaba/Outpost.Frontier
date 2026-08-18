#include "pch.h"
#include "CppUnitTest.h"

#include "ObjMesh.h"

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

  TEST_METHOD(OnlyFrigateLacksGlassAndTheRestSharePalettes)
  {
    // The corpus authors one palette across nine files; a mesh that drifts is a
    // content bug the renderer would show as a recolour and say nothing about.
    const std::string directory = FindMeshDirectory();
    const char* files[] = {"Interceptor.obj", "Bomber.obj",  "Corvette.obj",   "Frigate.obj", "Hauler.obj",
                           "Miner.obj",       "Carrier.obj", "Battleship.obj", "Structure.obj"};

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
