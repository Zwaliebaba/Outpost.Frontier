#pragma once

#include "GpuCom.h"
#include "NebulaField.h"

#include <d3d12.h>

#include <cstdint>

/*
 * The baked nebula field, on the GPU (ADR-006 §1).
 *
 * All this does is get one R8 tile from `NebulaField` into a texture and put an
 * SRV where the root signature's table expects it. The field's maths, its
 * parameters and every property worth asserting live in `NebulaField.h`, which
 * no GPU is needed to test; this file is the part that cannot be tested without
 * a device, and it is kept as small as that split allows.
 *
 * Upload is the same shape as `GlyphAtlas`: create in COPY_DEST, fill an upload
 * buffer, copy, transition to PIXEL_SHADER_RESOURCE, wait once, and let the
 * staging buffer die. Once at boot, never again -- which is what lets the
 * descriptor range be DATA_STATIC.
 */

namespace Neuron
{

class GpuDevice;

class GpuNebula
{
public:
  GpuNebula() = default;
  ~GpuNebula();

  GpuNebula(const GpuNebula&) = delete;
  GpuNebula& operator=(const GpuNebula&) = delete;

  /// Bakes the field and uploads it, writing the SRV to `_srvDestination`.
  /// Returns false if the settings do not describe a field; the caller decides
  /// whether that is fatal.
  [[nodiscard]] bool Create(GpuDevice& _device, const NebulaSettings& _settings, D3D12_CPU_DESCRIPTOR_HANDLE _srvDestination);
  void Destroy();

  [[nodiscard]] bool Ready() const noexcept { return m_texture != nullptr; }

  /// What the shader needs that the texture does not carry: the tint, the
  /// intensity and the tile size the sampler's wrap is measured in.
  [[nodiscard]] const NebulaSettings& Settings() const noexcept { return m_settings; }

  [[nodiscard]] std::uint32_t Resolution() const noexcept { return m_resolution; }

private:
  GpuPtr<ID3D12Resource> m_texture;
  NebulaSettings m_settings;
  std::uint32_t m_resolution = 0;
};

} // namespace Neuron
