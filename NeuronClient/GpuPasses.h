#pragma once

#include <d3d12.h>

#include <cstdint>

/*
 * The frame, as a fixed list of named passes (ADR-006 §1).
 *
 * Not a frame graph. A frame graph exists to schedule transient resources
 * across many passes; this frame has four, and their resources are one render
 * target, one depth buffer and one upload ring. What it is instead: a list of
 * structs with Record(context), executed in order on one direct queue, whose
 * names are the corpus's target frame with the unbuilt nodes simply absent.
 *
 *   Clear -> Opaque -> OverlayWorld -> Ui -> Present
 *
 * Slice S5 builds Clear and Opaque. OverlayWorld (S8) and Ui (S11) are declared
 * below as reserved slots, and so are the corpus's GpuCull, DepthPre, Effects,
 * Nebula and Tonemap nodes -- growth is an insertion into this list, which is
 * the whole reason the list is written out rather than inlined into the frame
 * loop.
 *
 * Barriers are not a pass. The back buffer's PRESENT/RENDER_TARGET transitions
 * belong to whoever owns the swapchain -- the frame loop -- and a pass that
 * transitioned resources it does not own could not be reordered without
 * reading every other pass first.
 */

namespace Neuron
{

class GpuMeshTable;
class GpuPipelines;
class GpuUploadRing;
struct RenderScene;

/// Everything a pass is allowed to touch, handed in rather than reached for.
struct FrameContext
{
  ID3D12GraphicsCommandList* commandList = nullptr;
  GpuUploadRing* uploadRing = nullptr;
  const GpuPipelines* pipelines = nullptr;
  const GpuMeshTable* meshes = nullptr;
  const RenderScene* scene = nullptr;

  D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView{};
  D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView{};
  D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;
  D3D12_GPU_VIRTUAL_ADDRESS passConstants = 0;
  D3D12_GPU_DESCRIPTOR_HANDLE textureTable{};

  std::uint32_t viewportWidth = 0;
  std::uint32_t viewportHeight = 0;
  float clearColour[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

/// Binds the targets, sets the viewport, and clears colour and depth.
struct ClearPass
{
  void Record(const FrameContext& _context) const;
};

/// Instanced hulls, one draw per material per class (ADR-006 §6).
struct OpaquePass
{
  void Record(const FrameContext& _context);

  /// What the last Record actually issued, for the debug strip (S14).
  [[nodiscard]] std::uint32_t DrawCount() const noexcept { return m_drawCount; }
  [[nodiscard]] std::uint32_t InstanceCount() const noexcept { return m_instanceCount; }

private:
  std::uint32_t m_drawCount = 0;
  std::uint32_t m_instanceCount = 0;
};

/*
 * The pass list itself.
 *
 * Reserved slots, in the order they will be inserted:
 *   GpuCull    -- corpus target; a compute cull once instance counts justify it
 *   DepthPre   -- corpus target; also what turns OverlayWorld's hard depth test
 *                 into the soft SRV occlusion ADR-006 §8 wants
 *   Effects    -- corpus target
 *   Nebula     -- corpus target
 *   Tonemap    -- arrives with HDR; the SDR path has nothing to tone-map
 *   OverlayWorld (S8), Ui (S11)
 */
class GpuPassList
{
public:
  void Record(const FrameContext& _context);

  [[nodiscard]] const OpaquePass& Opaque() const noexcept { return m_opaque; }

private:
  ClearPass m_clear;
  OpaquePass m_opaque;
};

} // namespace Neuron
