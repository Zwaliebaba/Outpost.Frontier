#pragma once

#include "GpuCom.h"
#include "ObjMesh.h"

#include <d3d12.h>
#include <DirectXMath.h>

#include <cstdint>
#include <span>

/*
 * Root signature, shaders and pipeline states (ADR-006 §12).
 *
 * Four PSOs at MVP scale -- opaque, overlay-instanced, overlay-polyline,
 * ui/text -- of which S5 builds the first. They share one root signature, which
 * is why it lives here rather than beside a pass: a root signature per pass
 * would mean rebinding descriptor tables at every pass boundary for no gain at
 * this size.
 *
 * Shaders arrive compiled, as bytes, from the composition root. This project
 * does not read them, name them or know where they came from -- the same
 * arrangement `WorldView` gave the world in S5c, for the same reason: the
 * engine ships around a second game (ADR-014), and a hard-coded shader path is
 * a game-shaped fact.
 *
 * They used to be HLSL under `GameData/Shaders`, compiled at boot by
 * `D3DCompileFromFile`, on the argument that a shader is content like a mesh
 * and an edit should be a restart rather than a rebuild. Owner directive: they
 * are built now, by `fxc` as part of `Outpost.vcxproj`, into byte arrays the
 * executable carries. The trade is deliberate -- a shader edit is a rebuild
 * again, and in exchange a broken shader fails the build in CI instead of the
 * client at boot on a machine with a GPU.
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
  DirectX::XMFLOAT4 bounceDirection;
  DirectX::XMFLOAT4 bounceColour;
  DirectX::XMFLOAT4 materialAlbedo[MESH_MATERIAL_COUNT];
  DirectX::XMFLOAT4 teamEmissive[TEAM_COLOUR_COUNT];

  /// x = seconds since the client started, for the signal lamps' animation
  /// (ADR-006 §6a); yzw reserved. Appended after the arrays, so nothing above
  /// it moved.
  DirectX::XMFLOAT4 frameTime;
};

/*
 * Mirrors `cbuffer PassConstants : register(b1)`, declared once in
 * `Outpost/Shaders/PassConstants.hlsli` and included by every shader that binds
 * the slot. Two declarations of one cbuffer disagreeing is a bug with no error
 * message anywhere; there is one on the HLSL side and this one here, and the
 * pair is edited together or not at all.
 *
 * The plane mapping is three float2s in float4 slots. The padding is deliberate:
 * packing them into two registers saves 16 bytes on a buffer written once a
 * frame and costs a reader the ability to see what a field is.
 */
struct PassConstants
{
  DirectX::XMFLOAT4 viewportSize;     // xy = pixels, zw = 1/pixels.
  DirectX::XMFLOAT4 planeAxes;        // xy = screen-right on the plane, zw = screen-up.
  DirectX::XMFLOAT4 planeOrigin;      // xy = the plane point at the centre of the view.
  DirectX::XMFLOAT4 planeRightPerNdc; // xy = plane metres per unit of NDC x.
  DirectX::XMFLOAT4 planeUpPerNdc;    // xy = plane metres per unit of NDC y.
  DirectX::XMFLOAT4 nebulaTint;       // rgb = linear tint, w = peak intensity.
  DirectX::XMFLOAT4 nebulaTile;       // x = 1 / tile metres.
};

/*
 * The compiled shaders, borrowed.
 *
 * Spans over byte arrays the caller owns -- in practice `Outpost/CompiledShaders`
 * headers, which have static storage duration and outlive everything. Nothing
 * here is copied, so a caller handing over a local buffer would be handing over
 * a dangling one; the contract is the same one `ClientApp` has with its
 * `WorldView`.
 *
 * Named for the stage rather than the file, because the file is the caller's
 * business. An empty span is a missing shader and `Create` refuses on it: a PSO
 * built from nothing is a device-removed crash several frames later.
 */
struct PipelineShaders
{
  std::span<const std::uint8_t> opaqueVertex;
  std::span<const std::uint8_t> opaquePixel;
  std::span<const std::uint8_t> nebulaVertex;
  std::span<const std::uint8_t> nebulaPixel;
  std::span<const std::uint8_t> overlayVertex;
  std::span<const std::uint8_t> overlayPixel;
  std::span<const std::uint8_t> uiVertex;
  std::span<const std::uint8_t> uiPixel;

  /// The signal lamps (ADR-006 §6a). Empty is a client with no lamp pipeline,
  /// which draws no lamps and boots -- unlike the four stages `AllPresent`
  /// gates on, a missing glow is a look rather than a broken frame.
  std::span<const std::uint8_t> lampVertex;
  std::span<const std::uint8_t> lampPixel;
};

/// Root parameter slots. Named because a bare index at a Set call site is the
/// easiest thing in D3D12 to get silently wrong.
enum class RootSlot : std::uint32_t
{
  FrameConstants = 0,
  PassConstants = 1,
  DrawConstants = 2,
  Textures = 3,

  /// The signal lamp table at b3 (ADR-006 §6a) -- a root CBV over a buffer
  /// written once at boot, bound only by the lamp pass. A slot of its own
  /// rather than a growth of `FrameConstants`, because it is the one constant
  /// block in the frame that is *not* per frame, and putting five kilobytes of
  /// static data through the upload ring sixty times a second to avoid one root
  /// parameter is the wrong trade.
  LampConstants = 4
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

  /*
   * `_sampleCount` is the world passes' MSAA count -- 1, or the swapchain's
   * offscreen target's 4 (ADR-006 §13, S14). The opaque, nebula and overlay
   * pipelines take it because they draw before the resolve; the Ui pipeline
   * always builds single-sampled, because the HUD draws on the resolved back
   * buffer where glyph edges land on pixel boundaries anyway.
   */
  [[nodiscard]] bool Create(ID3D12Device* _device, const PipelineShaders& _shaders, std::uint32_t _sampleCount = 1);
  void Destroy();

  [[nodiscard]] ID3D12RootSignature* RootSignature() const noexcept { return m_rootSignature.get(); }
  [[nodiscard]] ID3D12PipelineState* Opaque() const noexcept { return m_opaque.get(); }
  [[nodiscard]] ID3D12PipelineState* Nebula() const noexcept { return m_nebula.get(); }

  /*
   * Two overlay pipelines, one shader pair (ADR-006 §8).
   *
   * They differ in depth state and nothing else. A selection ring lies on the
   * plane and depth-tests against hulls, so a ring behind a Carrier is occluded
   * by it; a bar faces the screen and never occludes, because a health readout
   * that hides behind the thing it describes is not a readout. That is
   * `overlay-pass.png`'s rule, and it is a property of the pipeline rather than
   * of the shader -- which is why the marks are sorted rings-then-bars and
   * drawn as two ranges.
   */
  [[nodiscard]] ID3D12PipelineState* OverlayRings() const noexcept { return m_overlayRings.get(); }
  [[nodiscard]] ID3D12PipelineState* OverlayBars() const noexcept { return m_overlayBars.get(); }

  /// One pipeline for the whole HUD: panels and glyphs are the same quad in the
  /// same space, and differ by a flag on the instance (ADR-006 §10).
  [[nodiscard]] ID3D12PipelineState* Ui() const noexcept { return m_ui.get(); }

  /// The Ui pipeline built for the *world* target: multisampled, and drawn
  /// before the hulls so they paint over it. What puts the ghost's lane behind
  /// the ships rather than across them.
  [[nodiscard]] ID3D12PipelineState* UiWorld() const noexcept { return m_uiWorld.get(); }

  /// The signal lamps (ADR-006 §6a): additive glow billboards, depth-tested
  /// against hulls and never depth-writing. Null when the composition root
  /// supplied no lamp shaders, which the pass reads as "draw nothing".
  [[nodiscard]] ID3D12PipelineState* Lamps() const noexcept { return m_lamps.get(); }

private:
  [[nodiscard]] bool CreateRootSignature(ID3D12Device* _device);
  [[nodiscard]] bool CreateOpaquePipeline(ID3D12Device* _device, const PipelineShaders& _shaders);
  [[nodiscard]] bool CreateNebulaPipeline(ID3D12Device* _device, const PipelineShaders& _shaders);
  [[nodiscard]] bool CreateOverlayPipelines(ID3D12Device* _device, const PipelineShaders& _shaders);
  [[nodiscard]] bool CreateUiPipeline(ID3D12Device* _device, const PipelineShaders& _shaders);
  [[nodiscard]] bool CreateLampPipeline(ID3D12Device* _device, const PipelineShaders& _shaders);

  /// The world passes' sample count, stored between Create's helpers. 1 or the
  /// MSAA count; never read after Create returns.
  std::uint32_t m_sampleCount = 1;

  GpuPtr<ID3D12RootSignature> m_rootSignature;
  GpuPtr<ID3D12PipelineState> m_opaque;
  GpuPtr<ID3D12PipelineState> m_nebula;
  GpuPtr<ID3D12PipelineState> m_overlayRings;
  GpuPtr<ID3D12PipelineState> m_overlayBars;
  GpuPtr<ID3D12PipelineState> m_ui;
  GpuPtr<ID3D12PipelineState> m_uiWorld;
  GpuPtr<ID3D12PipelineState> m_lamps;
};

} // namespace Neuron
