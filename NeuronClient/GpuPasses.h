#pragma once

#include "UiDrawList.h"

#include <d3d12.h>

#include <cstdint>
#include <vector>

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

class GlyphAtlas;
class GpuMeshTable;
class GpuPipelines;
class GpuUploadRing;
struct OverlayMarkList;
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

  /// The frame's HUD, built by the client into screen-space quads and text runs.
  /// Null or empty draws nothing, which is what a client with no HUD looks like.
  const UiDrawList* ui = nullptr;

  /// The screen-space marks that belong *under* the hulls -- the ghost's lane.
  /// Drawn into the world target before the Opaque pass, so the ships paint
  /// over it. Null is a frame with no order in flight.
  const UiDrawList* uiWorld = nullptr;

  /// The baked glyphs the Ui pass turns text runs into quads against. Null
  /// draws the panels and drops the text, which is a HUD that boots without a
  /// font rather than one that does not boot.
  const GlyphAtlas* glyphAtlas = nullptr;

  /// The frame's selection rings and gauge bars, built by `BuildOverlayMarks`.
  /// Null or empty draws nothing, which is what an empty selection looks like.
  const OverlayMarkList* overlayMarks = nullptr;

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
 * Selection rings and gauge bars (ADR-006 §8).
 *
 * After the nebula, because the overlay is a readout and nothing may composite
 * over it. Two draws over one instance stream: the rings first, depth-tested so
 * one behind a Carrier is occluded by it, then the bars with no depth at all so
 * a health readout never hides behind the ship it describes. That split is why
 * `OverlayMarkList` keeps its rings contiguous.
 *
 * One upload, two draws, four vertices each -- the quads come from
 * `SV_VertexID`, so the only thing in the vertex buffer is the marks.
 */
struct OverlayWorldPass
{
  void Record(const FrameContext& _context);

  /// What the last Record issued, for the debug strip (S14) and for a test that
  /// wants to know the pass ran rather than that it looked right.
  [[nodiscard]] std::uint32_t RingCount() const noexcept { return m_ringCount; }
  [[nodiscard]] std::uint32_t BarCount() const noexcept { return m_barCount; }

private:
  std::uint32_t m_ringCount = 0;
  std::uint32_t m_barCount = 0;
};

/*
 * The HUD (ADR-006 §9, §10).
 *
 * Last, and over everything: it is a readout, and the one thing that may
 * composite over the world because it *is* the thing the player reads. No depth
 * buffer is bound at all -- see `GpuPipelines::CreateUiPipeline`.
 *
 * One upload and one draw for the whole HUD. Panels and glyphs are the same
 * instance stream and differ by a flag, so the natural build order (panel, text,
 * panel, text) survives into the draw without a sort. Expanding text runs into
 * glyph quads happens here because it is the only step that needs the atlas;
 * everything upstream of it is device-free and tested without one.
 */
struct UiPass
{
  /*
   * `_worldLayer` picks which half of the HUD this instance draws.
   *
   * False is the HUD proper: `context.ui`, the single-sampled pipeline, on the
   * resolved back buffer, last. True is the world layer -- `context.uiWorld`,
   * the multisampled pipeline, into the world target *before* the hulls, so
   * they paint over it. One pass type rather than two because the work is
   * identical; two instances because each keeps its own instance buffer.
   */
  void Record(const FrameContext& _context, bool _worldLayer = false);

  /// What the last Record issued. The panels and the glyphs separately, because
  /// "the HUD drew nothing" and "the HUD drew boxes with no words in them" are
  /// different failures and the debug strip (S14) should not conflate them.
  [[nodiscard]] std::uint32_t PanelCount() const noexcept { return m_panelCount; }
  [[nodiscard]] std::uint32_t GlyphCount() const noexcept { return m_glyphCount; }

  /// Characters the atlas had no glyph for. Non-zero means the HUD is asking
  /// for something the bake did not cover -- a box-drawing character, say --
  /// and the fix is the bake's alphabet, not the layout.
  [[nodiscard]] std::uint64_t MissingGlyphCount() const noexcept { return m_missingGlyphs; }

private:
  std::vector<UiInstance> m_instances; // Reused across frames.
  std::uint32_t m_panelCount = 0;
  std::uint32_t m_glyphCount = 0;
  std::uint64_t m_missingGlyphs = 0;
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
 */
class GpuPassList
{
public:
  /*
   * Two halves rather than one Record (S14): everything up to and including
   * OverlayWorld draws into `renderTargetView` -- the MSAA target when one
   * exists -- and Ui draws after the frame loop has resolved, straight onto
   * the back buffer. The resolve and its barriers sit between the two calls
   * and belong to the frame loop, exactly as the PRESENT transitions do:
   * barriers are not a pass (see the file comment).
   */
  void RecordWorld(const FrameContext& _context);
  void RecordUi(const FrameContext& _context);

  [[nodiscard]] const OpaquePass& Opaque() const noexcept { return m_opaque; }
  [[nodiscard]] const NebulaPass& Nebula() const noexcept { return m_nebula; }
  [[nodiscard]] const OverlayWorldPass& OverlayWorld() const noexcept { return m_overlayWorld; }
  [[nodiscard]] const UiPass& Ui() const noexcept { return m_ui; }

private:
  ClearPass m_clear;
  /// The world layer of the HUD, drawn before the hulls so they cover it. Its
  /// own instance because a `UiPass` owns the instance buffer it uploads.
  UiPass m_uiWorldLayer;
  OpaquePass m_opaque;
  NebulaPass m_nebula;
  OverlayWorldPass m_overlayWorld;
  UiPass m_ui;
};

} // namespace Neuron
