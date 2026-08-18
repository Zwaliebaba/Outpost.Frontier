#pragma once

#include "GpuCom.h"
#include "ObjMesh.h"

#include <d3d12.h>
#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <string_view>

/*
 * Root signature, shaders and pipeline states (ADR-006 §12).
 *
 * Four PSOs at MVP scale -- opaque, overlay-instanced, overlay-polyline,
 * ui/text -- of which S5 builds the first. They share one root signature, which
 * is why it lives here rather than beside a pass: a root signature per pass
 * would mean rebinding descriptor tables at every pass boundary for no gain at
 * this size.
 *
 * Shaders are HLSL under GameData/Shaders and are compiled at boot. They are
 * content, like the meshes and the universe file: a shader edit is a restart,
 * not a rebuild, and a compile error is a diagnostic with the file and line in
 * it rather than a build break in a project nobody was touching.
 */

namespace Neuron
{

inline constexpr std::uint32_t TEAM_COLOUR_COUNT = 4;

/*
 * Mirrors `cbuffer FrameConstants : register(b0)` in Opaque.hlsl.
 *
 * HLSL packs a float4 array one element per 16-byte register and a row_major
 * float4x4 as four consecutive registers, which is exactly what these types
 * are -- so the layout matches by construction rather than by padding fields.
 */
struct FrameConstants
{
  DirectX::XMFLOAT4X4 viewProjection;
  DirectX::XMFLOAT4 sunDirection;
  DirectX::XMFLOAT4 sunColour;
  DirectX::XMFLOAT4 ambientSky;
  DirectX::XMFLOAT4 ambientGround;
  DirectX::XMFLOAT4 materialAlbedo[MESH_MATERIAL_COUNT];
  DirectX::XMFLOAT4 teamEmissive[TEAM_COLOUR_COUNT];
};

/// Mirrors `cbuffer PassConstants : register(b1)`.
struct PassConstants
{
  DirectX::XMFLOAT4 viewportSize;
  DirectX::XMFLOAT4 planeAxes;
};

/// Root parameter slots. Named because a bare index at a Set call site is the
/// easiest thing in D3D12 to get silently wrong.
enum class RootSlot : std::uint32_t
{
  FrameConstants = 0,
  PassConstants = 1,
  DrawConstants = 2,
  Textures = 3
};

class GpuPipelines
{
public:
  static constexpr DXGI_FORMAT RENDER_TARGET_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  static constexpr DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_D32_FLOAT;

  GpuPipelines() = default;
  ~GpuPipelines();

  GpuPipelines(const GpuPipelines&) = delete;
  GpuPipelines& operator=(const GpuPipelines&) = delete;

  [[nodiscard]] bool Create(ID3D12Device* _device, std::string_view _shaderDirectory);
  void Destroy();

  [[nodiscard]] ID3D12RootSignature* RootSignature() const noexcept { return m_rootSignature.get(); }
  [[nodiscard]] ID3D12PipelineState* Opaque() const noexcept { return m_opaque.get(); }

private:
  [[nodiscard]] bool CreateRootSignature(ID3D12Device* _device);
  [[nodiscard]] bool CreateOpaquePipeline(ID3D12Device* _device, std::string_view _shaderDirectory);

  GpuPtr<ID3D12RootSignature> m_rootSignature;
  GpuPtr<ID3D12PipelineState> m_opaque;
};

} // namespace Neuron
