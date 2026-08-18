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
 *   Clear -> Opaque -> Nebula -> OverlayWorld -> Ui -> Present
 *
 * Slice S5 built Clear and Opaque; Nebula is the first insertion into the
 * reserved list, and it landed exactly as an insertion -- one struct, one line
 * in Record, and nothing else in this file moved. That was the claim the list
 * was written out for instead of being inlined into the frame loop, and it is
 * now a measured claim rather than a hopeful one. OverlayWorld (S8) and Ui (S11)
 * are still to come, as are the corpus's GpuCull, DepthPre, Effects and Tonemap.
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

  /// Whether the nebula field baked and uploaded. The pass reads its tint and
  /// tile size out of the pass constants, so this is the only thing it needs to
  /// be told directly: is there a texture at t1 or not.
  bool nebulaReady = false;
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
 * The ambient field the fleet sits inside (ADR-006 §1).
 *
 * After Opaque, not before it: the corpus frame composites the nebula over the
 * geometry, which is what makes hulls read as being inside the cloud rather
 * than pasted onto a backdrop. One additive full-screen triangle, no vertex
 * buffer, no depth opinion.
 *
 * It draws nothing if the field never baked. That is a real state -- a bad
 * `nebula` block in the configuration -- and the answer to it is a frame
 * without haze, not a client that will not start.
 */
struct NebulaPass
{
  void Record(const FrameContext& _context);

  /// False when the field is missing, so the debug strip (S14) can say why the
  /// background is flat rather than leaving someone to wonder.
  [[nodiscard]] bool Drew() const noexcept { return m_drew; }

private:
  bool m_drew = false;
};

/*
 * The pass list itself.
 *
 * Reserved slots, in the order they will be inserted:
 *   GpuCull    -- corpus target; a compute cull once instance counts justify it
 *   DepthPre   -- corpus target; also what turns OverlayWorld's hard depth test
 *                 into the soft SRV occlusion ADR-006 §8 wants
 *   Effects    -- corpus target
 *   Tonemap    -- arrives with HDR; the SDR path has nothing to tone-map
 *   OverlayWorld (S8), Ui (S11)
 */
class GpuPassList
{
public:
  void Record(const FrameContext& _context);

  [[nodiscard]] const OpaquePass& Opaque() const noexcept { return m_opaque; }
  [[nodiscard]] const NebulaPass& Nebula() const noexcept { return m_nebula; }

private:
  ClearPass m_clear;
  OpaquePass m_opaque;
  NebulaPass m_nebula;
};

} // namespace Neuron
