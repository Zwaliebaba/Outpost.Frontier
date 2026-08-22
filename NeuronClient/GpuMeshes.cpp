#include "pch.h"

#include "GpuMeshes.h"

#include "GpuDevice.h"
#include "Log.h"
#include "TaskPool.h"
#include "Telemetry.h"

#include <cstring>

namespace Neuron
{
namespace
{

/*
 * A buffer on the given heap, in the only state that heap allows.
 *
 * **The state is derived rather than passed**, because for a buffer there is no
 * choice to make and offering one is how the wrong answer gets written:
 *
 *  - `DEFAULT` -- **COMMON**. Buffers are effectively always created in COMMON
 *    whatever is asked for, and the debug layer says so
 *    (`CREATERESOURCE_STATE_IGNORED`, #1328). This function used to be handed
 *    `COPY_DEST` by the mesh upload, which reads correctly, matches the sample
 *    in the D3D12 docs, and is ignored -- so every mesh buffer produced a
 *    warning that was easy to read as noise. Common-state promotion is what
 *    makes it harmless: a buffer in COMMON is promoted to `COPY_DEST` by the
 *    copy itself, so the transition afterwards is still valid.
 *  - `UPLOAD` -- **GENERIC_READ**, which the API requires rather than ignores.
 *  - `READBACK` -- **COPY_DEST**, likewise required. Nothing reads back yet;
 *    it is here so the next caller does not have to look it up.
 */
[[nodiscard]] GpuPtr<ID3D12Resource> CreateBuffer(ID3D12Device* _device, std::uint64_t _bytes, D3D12_HEAP_TYPE _heapType)
{
  D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
  if (_heapType == D3D12_HEAP_TYPE_UPLOAD)
  {
    initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
  }
  else if (_heapType == D3D12_HEAP_TYPE_READBACK)
  {
    initialState = D3D12_RESOURCE_STATE_COPY_DEST;
  }

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = _heapType;

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = _bytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  GpuPtr<ID3D12Resource> buffer;
  check_hresult(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(buffer.put())));
  return buffer;
}

void CopyThroughStaging(ID3D12Device* _device, ID3D12GraphicsCommandList* _commandList, const void* _data, std::uint64_t _bytes,
                        ID3D12Resource* _destination, D3D12_RESOURCE_STATES _finalState,
                        std::vector<GpuPtr<ID3D12Resource>>& _staging)
{
  GpuPtr<ID3D12Resource> upload = CreateBuffer(_device, _bytes, D3D12_HEAP_TYPE_UPLOAD);

  void* mapped = nullptr;
  const D3D12_RANGE readRange{0, 0}; // Write-only: the CPU never reads this back.
  check_hresult(upload->Map(0, &readRange, &mapped));
  std::memcpy(mapped, _data, static_cast<std::size_t>(_bytes));
  upload->Unmap(0, nullptr);

  _commandList->CopyBufferRegion(_destination, 0, upload.get(), 0, _bytes);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = _destination;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  // COPY_DEST is where the copy above promoted it from COMMON, which is the
  // state buffers are actually created in whatever `CreateCommittedResource`
  // was told. Promotion is automatic for buffers, so this transition is exactly
  // as valid as it was when the buffer asked to be created in COPY_DEST.
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = _finalState;
  _commandList->ResourceBarrier(1, &barrier);

  // Held by the caller until the copy has actually run. Releasing it here would
  // free memory the GPU is about to read.
  _staging.push_back(std::move(upload));
}

} // namespace

GpuMeshTable::~GpuMeshTable()
{
  Destroy();
}

bool GpuMeshTable::UploadMesh(GpuDevice& _device, ID3D12GraphicsCommandList* _commandList, const ObjMesh& _source, GpuMesh& _outMesh,
                              std::vector<GpuPtr<ID3D12Resource>>& _staging)
{
  const auto vertexBytes = static_cast<std::uint64_t>(_source.vertices.size() * sizeof(MeshVertex));
  const auto indexBytes = static_cast<std::uint64_t>(_source.indices.size() * sizeof(std::uint32_t));
  if (vertexBytes == 0 || indexBytes == 0)
  {
    return false;
  }

  _outMesh.vertexBuffer = CreateBuffer(_device.Device(), vertexBytes, D3D12_HEAP_TYPE_DEFAULT);
  _outMesh.indexBuffer = CreateBuffer(_device.Device(), indexBytes, D3D12_HEAP_TYPE_DEFAULT);
  NAME_D3D12_OBJECT(_outMesh.vertexBuffer);
  NAME_D3D12_OBJECT(_outMesh.indexBuffer);

  CopyThroughStaging(_device.Device(), _commandList, _source.vertices.data(), vertexBytes, _outMesh.vertexBuffer.get(),
                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, _staging);
  CopyThroughStaging(_device.Device(), _commandList, _source.indices.data(), indexBytes, _outMesh.indexBuffer.get(),
                     D3D12_RESOURCE_STATE_INDEX_BUFFER, _staging);

  _outMesh.vertexView.BufferLocation = _outMesh.vertexBuffer->GetGPUVirtualAddress();
  _outMesh.vertexView.SizeInBytes = static_cast<UINT>(vertexBytes);
  _outMesh.vertexView.StrideInBytes = static_cast<UINT>(sizeof(MeshVertex));

  _outMesh.indexView.BufferLocation = _outMesh.indexBuffer->GetGPUVirtualAddress();
  _outMesh.indexView.SizeInBytes = static_cast<UINT>(indexBytes);
  _outMesh.indexView.Format = DXGI_FORMAT_R32_UINT;

  _outMesh.submeshes = _source.submeshes;
  _outMesh.vertexCount = static_cast<std::uint32_t>(_source.vertices.size());
  _outMesh.indexCount = static_cast<std::uint32_t>(_source.indices.size());
  _outMesh.radiusMetres = _source.radiusMetres;
  _outMesh.boundsMin = _source.boundsMin;
  _outMesh.boundsMax = _source.boundsMax;
  return true;
}

bool GpuMeshTable::Create(GpuDevice& _device, std::string_view _directory, std::span<const std::string> _fileNames,
                          std::span<const float> _planeRadiiMetres, TaskPool& _taskPool)
{
  NEURON_SPAN("MeshLoad");
  Destroy();

  const auto count = static_cast<std::uint32_t>(_fileNames.size());
  if (count == 0)
  {
    NEURON_LOG_ERROR("no meshes configured -- the client has nothing to draw");
    return false;
  }

  // Parse in parallel, upload in order. The results vector is sized up front so
  // each task writes its own slot and no task touches another's.
  std::vector<ObjMesh> parsed(count);
  std::vector<ObjDiagnostic> errors(count);
  std::vector<std::uint8_t> succeeded(count, 0);

  WaitGroup group;
  for (std::uint32_t i = 0; i < count; ++i)
  {
    _taskPool.Submit(
        [&, i]()
        {
          NEURON_SPAN("MeshParse");
          succeeded[i] = LoadObjMesh(_directory, _fileNames[i], parsed[i], errors[i]) ? std::uint8_t{1} : std::uint8_t{0};
        },
        &group);
  }
  _taskPool.Wait(group);

  bool allLoaded = true;
  for (std::uint32_t i = 0; i < count; ++i)
  {
    if (succeeded[i] == 0)
    {
      NEURON_LOG_ERROR("%s", errors[i].Text().c_str());
      allLoaded = false;
    }
  }
  if (!allLoaded)
  {
    return false;
  }

  /*
   * Sized to what the caller says these things are, before anything is
   * uploaded (`FitObjMeshToPlaneRadius`).
   *
   * Here rather than in the parse tasks because it is not parsing, and after
   * the failure check rather than inside it because a mesh that did not load
   * has nothing to scale. The buffers are static and one per class, so this is
   * the last moment the geometry is on the CPU and the cheapest possible place
   * to do it: the scale is a per-class constant, so baking it into the vertices
   * costs nothing per frame and no instance field.
   *
   * Logged per mesh that actually moved, because a hull silently drawn at four
   * times the size it was exported at is exactly the kind of thing somebody
   * should be able to find in a log rather than by measuring a screenshot.
   */
  for (std::uint32_t i = 0; i < count; ++i)
  {
    const float target = i < _planeRadiiMetres.size() ? _planeRadiiMetres[i] : 0.0f;
    const float authored = PlaneRadiusMetres(parsed[i]);
    if (FitObjMeshToPlaneRadius(parsed[i], target))
    {
      NEURON_LOG_INFO("mesh %s: %.1f m authored -> %.1f m drawn (x%.2f)", _fileNames[i].c_str(),
                      static_cast<double>(authored), static_cast<double>(target),
                      static_cast<double>(authored > 0.0f ? target / authored : 1.0f));
    }
  }

  // One command list for every mesh: nine copies is one submission, and the
  // wait afterwards is boot time nobody is watching a frame during.
  GpuPtr<ID3D12CommandAllocator> allocator;
  GpuPtr<ID3D12GraphicsCommandList> commandList;
  check_hresult(_device.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())));
  check_hresult(_device.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr,
                                                    IID_PPV_ARGS(commandList.put())));

  std::vector<GpuPtr<ID3D12Resource>> staging;
  staging.reserve(static_cast<std::size_t>(count) * 2);

  m_meshes.resize(count);
  m_classRadii.resize(count);

  bool paletteTaken = false;
  for (std::uint32_t i = 0; i < count; ++i)
  {
    if (!UploadMesh(_device, commandList.get(), parsed[i], m_meshes[i], staging))
    {
      NEURON_LOG_ERROR("%s parsed but has no geometry", _fileNames[i].c_str());
      check_hresult(commandList->Close());
      Destroy();
      return false;
    }

    m_classRadii[i] = m_meshes[i].radiusMetres;
    m_totalTriangles += m_meshes[i].indexCount / 3;

    // The first file's palette wins and the rest are checked against it: the
    // corpus authors one palette across nine files, and a file that drifts is
    // a content bug worth a line in the log rather than a silent recolour.
    for (std::uint32_t material = 0; material < MESH_MATERIAL_COUNT; ++material)
    {
      if (!parsed[i].palette.defined[material])
      {
        continue;
      }
      if (!m_palette.defined[material])
      {
        m_palette.albedo[material] = parsed[i].palette.albedo[material];
        m_palette.defined[material] = true;
        paletteTaken = true;
      }
      else if (parsed[i].palette.albedo[material].x != m_palette.albedo[material].x ||
               parsed[i].palette.albedo[material].y != m_palette.albedo[material].y ||
               parsed[i].palette.albedo[material].z != m_palette.albedo[material].z)
      {
        NEURON_LOG_WARNING("%s defines a different '%s' colour; the first mesh's palette is the one that renders",
                           _fileNames[i].c_str(), MeshMaterialName(static_cast<MeshMaterial>(material)));
      }
    }
  }

  if (!paletteTaken)
  {
    NEURON_LOG_WARNING("no mesh defined any material colour; hulls will render black");
  }

  check_hresult(commandList->Close());
  ID3D12CommandList* lists[] = {commandList.get()};
  _device.Queue()->ExecuteCommandLists(1, lists);
  _device.WaitForIdle(); // Only here does the staging vector become safe to drop.

  NEURON_LOG_INFO("meshes: %u classes, %u triangles", count, m_totalTriangles);
  return true;
}

void GpuMeshTable::Destroy()
{
  m_meshes.clear();
  m_classRadii.clear();
  m_palette = MeshMaterialPalette{};
  m_totalTriangles = 0;
}

} // namespace Neuron
