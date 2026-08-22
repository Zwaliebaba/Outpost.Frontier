#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/*
 * OBJ/MTL loading (ADR-006 §5).
 *
 * Hand-rolled, because the format is ours: GameData/Meshes holds nine
 * triangulated, Y-up, forward = -Z meshes authored against five shared
 * materials. A general OBJ importer would be a dependency and most of it would
 * be dead code.
 *
 * Deliberately free of D3D12 and C++/WinRT headers, exactly as ClearColour.h
 * is: a parser is maths and text, so it has to be testable without a device
 * (Build Order S5 accepts it in NeuronClientTests). Turning the result into
 * vertex and index buffers is GpuMeshes.h's job.
 *
 * Malformed content is a diagnostic and never an assert (AGENTS.md §5): the
 * file is authored by a person, so the parser reports the line and column and
 * fails closed.
 */

namespace Neuron
{

/*
 * The five canonical materials (ADR-006 §6). Every mesh in the corpus is
 * authored against exactly these names, so the enum is the closed set and an
 * unknown `usemtl` is a content error rather than a sixth material.
 *
 * The order is the draw order within a mesh, and it is deliberate: opaque hull
 * work first, emissive accents last, so a submesh list reads the way the frame
 * records it.
 */
enum class MeshMaterial : std::uint8_t
{
  Hull,
  Plate,
  Glass,
  Accent,
  Thruster
};

inline constexpr std::uint32_t MESH_MATERIAL_COUNT = 5;

/// "hull" -> MeshMaterial::Hull. False for anything else -- the caller turns
/// that into a diagnostic, because only it knows the line it was reading.
[[nodiscard]] bool ParseMeshMaterialName(std::string_view _name, MeshMaterial& _outMaterial) noexcept;

[[nodiscard]] const char* MeshMaterialName(MeshMaterial _material) noexcept;

/*
 * One vertex, as the opaque pass wants it.
 *
 * Position and normal only: the corpus meshes carry texture coordinates, and
 * nothing in the MVP frame samples a texture on a hull, so they are parsed for
 * index correctness and then dropped rather than uploaded.
 *
 * Normals are per-face as authored (ADR-006 §5). The exporter has already
 * duplicated the corner vertices that need it, so plain vertex normals shade
 * flat with no shader work.
 */
struct MeshVertex
{
  DirectX::XMFLOAT3 position;
  DirectX::XMFLOAT3 normal;
};

/// A run of indices sharing one material -- one draw call in the opaque pass.
struct SubmeshRange
{
  std::uint32_t firstIndex = 0;
  std::uint32_t indexCount = 0;
  MeshMaterial material = MeshMaterial::Hull;
};

/// Diffuse colours as the .mtl authored them, indexed by MeshMaterial.
/// Emissive strength is not in the file: it is a renderer decision about which
/// canonical materials glow (ADR-006 §6), not something the exporter knows.
struct MeshMaterialPalette
{
  DirectX::XMFLOAT3 albedo[MESH_MATERIAL_COUNT] = {};
  bool defined[MESH_MATERIAL_COUNT] = {};
};

/*
 * A parsed mesh, ready to become one static vertex buffer and one index buffer.
 *
 * Indices are grouped by material, so `submeshes` partitions `indices` exactly:
 * ranges are contiguous, in canonical material order, and cover every index.
 */
struct ObjMesh
{
  std::vector<MeshVertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<SubmeshRange> submeshes;

  std::string materialLibrary; // The `mtllib` name, for the caller to resolve.
  MeshMaterialPalette palette;

  DirectX::XMFLOAT3 boundsMin = {0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 boundsMax = {0.0f, 0.0f, 0.0f};
  float radiusMetres = 0.0f; // About the origin, which is where the ship pivots.

  [[nodiscard]] std::uint32_t TriangleCount() const noexcept { return static_cast<std::uint32_t>(indices.size() / 3); }
};

/// Where a content file stopped making sense. Line and column are 1-based, as
/// every editor counts them.
struct ObjDiagnostic
{
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::string message;

  /// "Frigate.obj(12,4): unknown material 'chrome'"
  [[nodiscard]] std::string Text() const;
};

/// Parses OBJ text. On failure `_outError` says where, and `_outMesh` is not
/// to be trusted.
[[nodiscard]] bool ParseObjMesh(std::string_view _text, ObjMesh& _outMesh, ObjDiagnostic& _outError);

/// Parses MTL text into the canonical palette. An `Ns`/`Ks`/`d` line is read
/// past deliberately: the MVP shading model has no use for them.
[[nodiscard]] bool ParseMeshMaterials(std::string_view _text, MeshMaterialPalette& _outPalette, ObjDiagnostic& _outError);

/// Reads _directory/_fileName, then the .mtl its `mtllib` names from the same
/// directory. A missing or unreadable file is a diagnostic, not an exception.
[[nodiscard]] bool LoadObjMesh(std::string_view _directory, std::string_view _fileName, ObjMesh& _outMesh, ObjDiagnostic& _outError);

/*
 * How wide this mesh is *on the plane* -- the largest `hypot(x, z)` over its
 * vertices, about the origin the hull pivots on.
 *
 * The plane and not the sphere, because a silhouette is what a top-down camera
 * at a fixed elevation shows: a ring standing up in Y reads as its footprint
 * and not as its height, so measuring in three dimensions would call the
 * stargate half again as wide as the picture of it. It is also the number the
 * corpus already quotes when it sizes content against content (ADR-016 §10 --
 * "168 m of silhouette on the plane against the station's 253", which is this
 * function's answer for those two files to the metre).
 */
[[nodiscard]] float PlaneRadiusMetres(const ObjMesh& _mesh) noexcept;

/*
 * Scales a mesh about its origin so `PlaneRadiusMetres` comes out at
 * `_targetPlaneRadiusMetres`, and reports whether it did anything.
 *
 * **A content rule applied at load, rather than baked into the files.** The
 * game states how big every hull is -- it has to, because contact, formation
 * spacing and picking are all derived from that number -- while a mesh is
 * authored at whatever scale the modelling package handed back. Normalising
 * here means a re-export at any scale still lands correctly, and the rule is
 * *stated* somewhere rather than being a property somebody has to remember not
 * to break.
 *
 * The whole mesh moves together: positions scale, normals do not (a uniform
 * scale leaves them unit), and bounds and radius are recomputed rather than
 * multiplied, so nothing downstream can hold a stale opinion about how big the
 * result is.
 *
 * A non-positive target, or a mesh with no width on the plane, is left exactly
 * as authored and reported `false`. "No opinion" has to be expressible, or a
 * caller with nothing to say would scale a hull to a point.
 */
[[nodiscard]] bool FitObjMeshToPlaneRadius(ObjMesh& _mesh, float _targetPlaneRadiusMetres) noexcept;

/*
 * How tall the hull actually is *at one place on it* -- the highest vertex
 * inside a box of `_halfExtentX` by `_halfExtentZ` around (`_x`, `_z`).
 *
 * `boundsMax.y` answers a different question, and the difference is not
 * academic. A `Battleship`'s bounding box has its ceiling 100 m up because a
 * thin antenna reaches there; the deck out at the beam, where a navigation
 * lamp belongs, is 37 m. Anything anchoring a fitting to the hull's *surface*
 * needs the second number, and the first one is four lamp-widths of empty
 * space away from it (ADR-006 §6a).
 *
 * `_fallback` for a box with nothing in it -- a caller that probes off the end
 * of the hull gets an answer rather than a sentinel to test for.
 */
[[nodiscard]] float LocalTopMetres(const ObjMesh& _mesh, float _x, float _z, float _halfExtentX, float _halfExtentZ,
                                   float _fallback) noexcept;

} // namespace Neuron
