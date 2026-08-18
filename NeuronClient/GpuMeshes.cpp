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

/// A default-heap buffer plus the upload buffer that fills it. The upload
/// buffer outlives the call only until the copy fence signals, which is why the
/// caller holds them in a vector rather than this function.
[[nodiscard]] GpuPtr<ID3D12Resource> CreateBuffer(ID3D12Device* _device, std::uint64_t _bytes, D3D12_HEAP_TYPE _heapType,
                                                  D3D12_RESOURCE_STATES _initialState)
{
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
  check_hresult(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, _initialState, nullptr, IID_PPV_ARGS(buffer.put())));
  return buffer;
}

void CopyThroughStaging(ID3D12Device* _device, ID3D12GraphicsCommandList* _commandList, const void* _data, std::uint64_t _bytes,
                        ID3D12Resource* _destination, D3D12_RESOURCE_STATES _finalState,
                        std::vector<GpuPtr<ID3D12Resource>>& _staging)
{
  GpuPtr<ID3D12Resource> upload = CreateBuffer(_device, _bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

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

  _outMesh.vertexBuffer = CreateBuffer(_device.Device(), vertexBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  _outMesh.indexBuffer = CreateBuffer(_device.Device(), indexBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
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

bool GpuMeshTable::Create(GpuDevice& _device, std::string_view _directory, std::span<const std::string> _fileNames, TaskPool& _taskPool)
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
