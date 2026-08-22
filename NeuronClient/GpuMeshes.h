#pragma once

#include "GpuCom.h"
#include "ObjMesh.h"

#include <d3d12.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/*
 * The meshes, resident on the GPU (ADR-006 §5).
 *
 * One static vertex buffer and one static index buffer per class, in a default
 * heap, uploaded once at boot and never touched again. Submesh ranges come
 * straight from the loader, so a draw is one DrawIndexedInstanced per material
 * per class -- at most five per class, and four for a mesh with no glass.
 *
 * The table is indexed by `classId`, which is what `InstanceRecord` carries and
 * what the composition root's mesh list defines. Nothing here knows a Carrier
 * from a Hauler (ADR-014); it loads the file names it is handed, in order.
 *
 * Parsing is fanned out over the boot TaskPool because nine OBJ files is the
 * exact shape ADR-007 §4 sanctioned it for. The GPU upload is not: one command
 * list, one queue, and recording it from several threads would be machinery for
 * a copy that takes a millisecond.
 */

namespace Neuron
{

class GpuDevice;
class TaskPool;

struct GpuMesh
{
  GpuPtr<ID3D12Resource> vertexBuffer;
  GpuPtr<ID3D12Resource> indexBuffer;
  D3D12_VERTEX_BUFFER_VIEW vertexView{};
  D3D12_INDEX_BUFFER_VIEW indexView{};
  std::vector<SubmeshRange> submeshes;

  std::uint32_t vertexCount = 0;
  std::uint32_t indexCount = 0;
  float radiusMetres = 0.0f;
  DirectX::XMFLOAT3 boundsMin = {0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 boundsMax = {0.0f, 0.0f, 0.0f};

  [[nodiscard]] bool Valid() const noexcept { return indexCount != 0; }
};

class GpuMeshTable
{
public:
  GpuMeshTable() = default;
  ~GpuMeshTable();

  GpuMeshTable(const GpuMeshTable&) = delete;
  GpuMeshTable& operator=(const GpuMeshTable&) = delete;

  /*
   * Loads and uploads every file in _fileNames, in order: index i becomes
   * classId i. One unreadable or malformed file fails the whole call, with the
   * diagnostic already logged -- a fleet missing a hull is not a degraded mode
   * worth supporting.
   *
   * `_planeRadiiMetres` is how wide each of them should be **drawn** on the
   * plane, in the same order, and it is the caller's business entirely: this
   * table has no idea which index is a Carrier (ADR-014) and cannot know how
   * big one is. Each mesh is scaled about its origin to the radius it is given,
   * so the art may be authored at any scale and a re-export at a different one
   * still lands correctly.
   *
   * A zero, or a shorter span than the file list, means "as authored" for that
   * mesh. An empty span therefore loads exactly what the files contain, which
   * is what every caller that has no opinion should pass.
   */
  [[nodiscard]] bool Create(GpuDevice& _device, std::string_view _directory, std::span<const std::string> _fileNames,
                            std::span<const float> _planeRadiiMetres, TaskPool& _taskPool);
  void Destroy();

  [[nodiscard]] std::uint32_t Count() const noexcept { return static_cast<std::uint32_t>(m_meshes.size()); }
  [[nodiscard]] const GpuMesh& Mesh(std::uint32_t _classId) const noexcept { return m_meshes[_classId]; }

  /// Hull radius per classId, for the parked-fleet layout and, later, for the
  /// screen-floor clamp on selection rings.
  [[nodiscard]] std::span<const float> ClassRadii() const noexcept { return m_classRadii; }

  /// The five canonical colours. Every mesh in the corpus authors the same
  /// palette, so the table keeps one and says so when a file disagrees.
  [[nodiscard]] const MeshMaterialPalette& Palette() const noexcept { return m_palette; }

  [[nodiscard]] std::uint32_t TotalTriangleCount() const noexcept { return m_totalTriangles; }

private:
  [[nodiscard]] bool UploadMesh(GpuDevice& _device, ID3D12GraphicsCommandList* _commandList, const ObjMesh& _source, GpuMesh& _outMesh,
                                std::vector<GpuPtr<ID3D12Resource>>& _staging);

  std::vector<GpuMesh> m_meshes;
  std::vector<float> m_classRadii;
  MeshMaterialPalette m_palette;
  std::uint32_t m_totalTriangles = 0;
};

} // namespace Neuron
