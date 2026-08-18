#include "pch.h"

#include "GpuNebula.h"

#include "GpuDevice.h"
#include "Log.h"

#include <cstring>

using namespace winrt;

namespace Neuron
{

GpuNebula::~GpuNebula()
{
  Destroy();
}

bool GpuNebula::Create(GpuDevice& _device, const NebulaSettings& _settings, D3D12_CPU_DESCRIPTOR_HANDLE _srvDestination)
{
  Destroy();

  NebulaField field;
  if (!BakeNebulaField(_settings, field) || !field.Valid())
  {
    NEURON_LOG_ERROR("nebula: the configured field could not be baked (resolution %u, octaves %u, tile %.0f m)", _settings.resolution,
                     _settings.octaves, _settings.tileMetres);
    return false;
  }

  m_settings = _settings;
  m_resolution = field.resolution;

  // R8_UNORM, like the glyph atlas and for the same reason: the field is one
  // channel of intensity and the tint is applied in the shader, so recolouring
  // the haze is a config edit rather than a re-bake.
  D3D12_HEAP_PROPERTIES defaultHeap{};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC textureDesc{};
  textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  textureDesc.Width = field.resolution;
  textureDesc.Height = field.resolution;
  textureDesc.DepthOrArraySize = 1;
  textureDesc.MipLevels = 1;
  textureDesc.Format = DXGI_FORMAT_R8_UNORM;
  textureDesc.SampleDesc.Count = 1;
  textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

  check_hresult(_device.Device()->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(m_texture.put())));
  NAME_D3D12_OBJECT(m_texture);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rowCount = 0;
  UINT64 rowBytes = 0;
  UINT64 uploadBytes = 0;
  _device.Device()->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rowCount, &rowBytes, &uploadBytes);

  D3D12_HEAP_PROPERTIES uploadHeap{};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC uploadDesc{};
  uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  uploadDesc.Width = uploadBytes;
  uploadDesc.Height = 1;
  uploadDesc.DepthOrArraySize = 1;
  uploadDesc.MipLevels = 1;
  uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
  uploadDesc.SampleDesc.Count = 1;
  uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  GpuPtr<ID3D12Resource> upload;
  check_hresult(_device.Device()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(upload.put())));

  std::uint8_t* mapped = nullptr;
  const D3D12_RANGE readRange{0, 0};
  check_hresult(upload->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
  for (std::uint32_t row = 0; row < field.resolution; ++row)
  {
    // An upload buffer's row pitch is 256-byte aligned and the baked field is
    // tightly packed, so this copies a row at a time rather than the whole tile.
    std::memcpy(mapped + footprint.Offset + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                &field.intensity[static_cast<std::size_t>(row) * field.resolution], field.resolution);
  }
  upload->Unmap(0, nullptr);

  GpuPtr<ID3D12CommandAllocator> allocator;
  GpuPtr<ID3D12GraphicsCommandList> commandList;
  check_hresult(_device.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())));
  check_hresult(_device.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr,
                                                    IID_PPV_ARGS(commandList.put())));

  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = m_texture.get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  destination.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = upload.get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint = footprint;

  commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = m_texture.get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  commandList->ResourceBarrier(1, &barrier);

  check_hresult(commandList->Close());
  ID3D12CommandList* lists[] = {commandList.get()};
  _device.Queue()->ExecuteCommandLists(1, lists);
  _device.WaitForIdle(); // The upload buffer dies at the end of this scope.

  D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc{};
  viewDesc.Format = DXGI_FORMAT_R8_UNORM;
  viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  viewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  viewDesc.Texture2D.MipLevels = 1;
  _device.Device()->CreateShaderResourceView(m_texture.get(), &viewDesc, _srvDestination);

  NEURON_LOG_INFO("nebula: %ux%u field over %.0f m (%.0f m/texel), seed %u", m_resolution, m_resolution, m_settings.tileMetres,
                  m_settings.tileMetres / static_cast<float>(m_resolution), m_settings.seed);
  return true;
}

void GpuNebula::Destroy()
{
  m_texture = nullptr;
  m_resolution = 0;
}

} // namespace Neuron
