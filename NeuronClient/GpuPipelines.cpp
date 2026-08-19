#include "pch.h"

#include "GpuPipelines.h"

#include "Log.h"
#include "OverlayMark.h"
#include "RenderWorld.h"
#include "UiDrawList.h"

#include <cstddef>

namespace Neuron
{
namespace
{

/*
 * The bytes for one stage, in the shape D3D12 wants them.
 *
 * This is all that is left of what used to be a hundred lines of
 * `D3DCompileFromFile`, `ToWide` and error-blob printing. The shaders are
 * compiled by `fxc` as part of Outpost.vcxproj now, so a broken shader is a
 * build failure with a file and a line, in CI, on a machine with no GPU --
 * rather than a boot failure on a machine that has one.
 */
[[nodiscard]] D3D12_SHADER_BYTECODE Bytecode(std::span<const std::uint8_t> _code) noexcept
{
  return D3D12_SHADER_BYTECODE{_code.data(), _code.size()};
}

/// Every stage has to be present. A PSO built from an empty blob is not an
/// error at creation -- it is a device removal several frames later, with no
/// message naming the shader that was missing.
[[nodiscard]] bool AllPresent(const PipelineShaders& _shaders) noexcept
{
  const bool ok = !_shaders.opaqueVertex.empty() && !_shaders.opaquePixel.empty() && !_shaders.nebulaVertex.empty() &&
                  !_shaders.nebulaPixel.empty() && !_shaders.overlayVertex.empty() && !_shaders.overlayPixel.empty();
  if (!ok)
  {
    NEURON_LOG_ERROR("pipelines: the composition root supplied an empty shader; refusing to build a pipeline from nothing");
  }
  return ok;
}

} // namespace

GpuPipelines::~GpuPipelines()
{
  Destroy();
}

bool GpuPipelines::CreateRootSignature(ID3D12Device* _device)
{
  // One SRV table: t0 is the glyph atlas, t1 the baked nebula field, and
  // whatever the Ui and OverlayWorld passes sample tomorrow follows. DATA_STATIC
  // is the honest flag for both -- each texture and its descriptor are written
  // once at boot and never touched again, which is what the flag promises.
  D3D12_DESCRIPTOR_RANGE1 textureRange{};
  textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  textureRange.NumDescriptors = 2;
  textureRange.BaseShaderRegister = 0;
  textureRange.RegisterSpace = 0;
  textureRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
  textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER1 parameters[4]{};

  // Root CBVs rather than a table: two descriptors that change every frame, and
  // DATA_STATIC_WHILE_SET_AT_EXECUTE is the honest promise -- the upload ring
  // does not rewrite a frame's slice until the fence for it has signalled.
  D3D12_ROOT_PARAMETER1& frame = parameters[static_cast<std::uint32_t>(RootSlot::FrameConstants)];
  frame.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  frame.Descriptor.ShaderRegister = 0;
  frame.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
  frame.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_PARAMETER1& pass = parameters[static_cast<std::uint32_t>(RootSlot::PassConstants)];
  pass.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  pass.Descriptor.ShaderRegister = 1;
  pass.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
  pass.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  // Root constants, not a buffer: the material index changes at every draw, and
  // a CBV per submesh would be five allocations and five binds per class.
  D3D12_ROOT_PARAMETER1& draw = parameters[static_cast<std::uint32_t>(RootSlot::DrawConstants)];
  draw.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  draw.Constants.ShaderRegister = 2;
  draw.Constants.Num32BitValues = 1;
  draw.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_PARAMETER1& textures = parameters[static_cast<std::uint32_t>(RootSlot::Textures)];
  textures.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  textures.DescriptorTable.NumDescriptorRanges = 1;
  textures.DescriptorTable.pDescriptorRanges = &textureRange;
  textures.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // Two static samplers, both linear and neither costing a descriptor heap slot.
  // They differ only in addressing, and that difference is load-bearing: a glyph
  // sampled with wrap bleeds its neighbour into its edge, and a periodic field
  // sampled with clamp stops being periodic -- which is the whole reason
  // NebulaField bakes a tile that wraps.
  D3D12_STATIC_SAMPLER_DESC samplers[2]{};

  D3D12_STATIC_SAMPLER_DESC& clampSampler = samplers[0];
  clampSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  clampSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  clampSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  clampSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  clampSampler.MaxLOD = D3D12_FLOAT32_MAX;
  clampSampler.ShaderRegister = 0;
  clampSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC& wrapSampler = samplers[1];
  wrapSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  wrapSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  wrapSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  wrapSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  wrapSampler.MaxLOD = D3D12_FLOAT32_MAX;
  wrapSampler.ShaderRegister = 1;
  wrapSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = static_cast<UINT>(_countof(parameters));
  desc.Desc_1_1.pParameters = parameters;
  desc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(_countof(samplers));
  desc.Desc_1_1.pStaticSamplers = samplers;
  // Denying the stages the frame does not use keeps the signature small, and
  // the MVP has no tessellation, no geometry shader and no mesh pipeline.
  desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

  // Asked, not assumed: serialising a 1.1 signature for a runtime that only
  // speaks 1.0 fails at CreateRootSignature with nothing useful in the message.
  D3D12_FEATURE_DATA_ROOT_SIGNATURE highestVersion{};
  highestVersion.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
  if (FAILED(_device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &highestVersion, sizeof(highestVersion))))
  {
    highestVersion.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
  }

  GpuPtr<ID3DBlob> serialised;
  GpuPtr<ID3DBlob> errors;
  // The D3DX12 helper rather than D3D12SerializeVersionedRootSignature
  // directly: it reconstructs a 1.0 signature when 1.1 is unavailable, and that
  // fallback is the only reason to prefer either.
  const HRESULT result = D3DX12SerializeVersionedRootSignature(&desc, highestVersion.HighestVersion, serialised.put(), errors.put());
  if (FAILED(result))
  {
    if (errors != nullptr)
    {
      NEURON_LOG_ERROR("%s", static_cast<const char*>(errors->GetBufferPointer()));
    }
    return false;
  }

  check_hresult(_device->CreateRootSignature(0, serialised->GetBufferPointer(), serialised->GetBufferSize(),
                                             IID_PPV_ARGS(m_rootSignature.put())));
  NAME_D3D12_OBJECT(m_rootSignature);
  return true;
}

bool GpuPipelines::CreateOpaquePipeline(ID3D12Device* _device, const PipelineShaders& _shaders)
{
  // Slot 0 is the mesh, slot 1 is the instance stream. classId is not in the
  // layout: the draw call already knows the class, because the class is what
  // selected the mesh (RenderWorld.h).
  //
  // The offsets below are spelled by hand because that is what D3D12 asks for,
  // which makes them a second copy of InstanceRecord's layout. This is the
  // assert that keeps the two copies honest.
  static_assert(offsetof(InstanceRecord, posWorld) == 0, "INSTANCE_POSITION is declared at offset 0");
  static_assert(offsetof(InstanceRecord, heading) == 12, "INSTANCE_HEADING is declared at offset 12");
  static_assert(offsetof(InstanceRecord, teamColorId) == 16, "INSTANCE_CHANNELS is declared at offset 16");
  static_assert(offsetof(InstanceRecord, selectionAndLodBias) == 17, "INSTANCE_CHANNELS is two bytes wide");
  static_assert(offsetof(InstanceRecord, bank) == 20, "INSTANCE_BANK is declared at offset 20 -- appended, so nothing above it moved");
  static_assert(sizeof(MeshVertex) == 24 && offsetof(MeshVertex, normal) == 12, "NORMAL is declared at offset 12");

  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"INSTANCE_POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"INSTANCE_HEADING", 0, DXGI_FORMAT_R32_FLOAT, 1, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"INSTANCE_CHANNELS", 0, DXGI_FORMAT_R8G8_UINT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"INSTANCE_BANK", 0, DXGI_FORMAT_R32_FLOAT, 1, 20, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = m_rootSignature.get();
  desc.VS = Bytecode(_shaders.opaqueVertex);
  desc.PS = Bytecode(_shaders.opaquePixel);
  desc.InputLayout = {elements, static_cast<UINT>(_countof(elements))};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = RENDER_TARGET_FORMAT;
  desc.DSVFormat = DEPTH_FORMAT;
  desc.SampleDesc.Count = m_sampleCount; // The 4x offscreen target when MSAA is on (S14).
  desc.SampleMask = UINT_MAX;

  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
  // FALSE, and measured against the shipped meshes rather than assumed: under
  // the tree's left-handed convention (ADR-006 §3a) the corpus comes out
  // clockwise in NDC where it faces the camera, which is what D3D12 calls a
  // front face by default. Standardising on LH is what makes this the default
  // rather than an override.
  desc.RasterizerState.FrontCounterClockwise = FALSE;
  desc.RasterizerState.DepthClipEnable = TRUE;

  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

  desc.DepthStencilState.DepthEnable = TRUE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

  check_hresult(_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_opaque.put())));
  NAME_D3D12_OBJECT(m_opaque);
  return true;
}

/*
 * The Nebula node (ADR-006 §1), and the first insertion into the reserved list.
 *
 * No input layout and no vertex buffer: the vertex shader builds one oversized
 * triangle from SV_VertexID. Additive, because the field adds light to a scene
 * that is otherwise near-black -- and depth-blind, because it composites over
 * everything the Opaque pass drew, which is the position the corpus frame gives
 * it and the reason hulls read as being inside the cloud.
 */
bool GpuPipelines::CreateNebulaPipeline(ID3D12Device* _device, const PipelineShaders& _shaders)
{
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = m_rootSignature.get();
  desc.VS = Bytecode(_shaders.nebulaVertex);
  desc.PS = Bytecode(_shaders.nebulaPixel);
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = RENDER_TARGET_FORMAT;
  // Declared even though depth is disabled below: a DSV is bound when this pass
  // records, and a PSO claiming DXGI_FORMAT_UNKNOWN against a bound DSV is a
  // debug-layer error rather than a no-op.
  desc.DSVFormat = DEPTH_FORMAT;
  desc.SampleDesc.Count = m_sampleCount;
  desc.SampleMask = UINT_MAX;

  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  // The full-screen triangle has one winding and no back to cull; culling
  // nothing means its winding never has to agree with the meshes'.
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.FrontCounterClockwise = FALSE;
  desc.RasterizerState.DepthClipEnable = TRUE;

  D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = D3D12_BLEND_ONE;
  blend.DestBlend = D3D12_BLEND_ONE;
  blend.BlendOp = D3D12_BLEND_OP_ADD;
  // Colour only. The back buffer's alpha is not the haze's business, and
  // accumulating into it would leave the flip model a surface it can present
  // differently depending on the compositor.
  blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
  blend.DestBlendAlpha = D3D12_BLEND_ONE;
  blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

  desc.DepthStencilState.DepthEnable = FALSE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

  check_hresult(_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_nebula.put())));
  NAME_D3D12_OBJECT(m_nebula);
  return true;
}

/*
 * The two OverlayWorld pipelines (ADR-006 §8).
 *
 * One shader pair, two depth states, and the difference is the whole rule the
 * `overlay-pass.png` acceptance is about: plane-lying marks depth-test against
 * hulls so a ring behind a Carrier is occluded by it, and screen-facing marks
 * never occlude because a bar that hides behind its own ship is not a readout.
 *
 * No vertex buffer for geometry: the quad comes from `SV_VertexID`, the same
 * trick the nebula's full-screen triangle uses, so slot 0 is the instance
 * stream and there is no slot 1.
 */
bool GpuPipelines::CreateOverlayPipelines(ID3D12Device* _device, const PipelineShaders& _shaders)
{
  // Four elements rather than seven, because the four size floats are
  // contiguous and mean "size" between them. The offsets are spelled by hand
  // because that is what D3D12 asks for, which makes them a second copy of
  // OverlayMark's layout -- and these are the asserts that keep the copies
  // honest.
  static_assert(offsetof(OverlayMark, anchorPlane) == 0, "MARK_ANCHOR is declared at offset 0");
  static_assert(offsetof(OverlayMark, radiusMetres) == 8, "MARK_SIZE starts at offset 8");
  static_assert(offsetof(OverlayMark, halfWidthPixels) == 12, "MARK_SIZE.y is the half width");
  static_assert(offsetof(OverlayMark, halfHeightPixels) == 16, "MARK_SIZE.z is the half height");
  static_assert(offsetof(OverlayMark, offsetUpPixels) == 20, "MARK_SIZE.w is the screen offset");
  static_assert(offsetof(OverlayMark, colourRgba) == 24, "MARK_COLOUR is declared at offset 24");
  static_assert(offsetof(OverlayMark, kind) == 28 && offsetof(OverlayMark, fill) == 30,
                "MARK_KINDFILL is two 16-bit lanes at offset 28");

  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"MARK_ANCHOR", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"MARK_SIZE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"MARK_COLOUR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"MARK_KINDFILL", 0, DXGI_FORMAT_R16G16_UINT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = m_rootSignature.get();
  desc.VS = Bytecode(_shaders.overlayVertex);
  desc.PS = Bytecode(_shaders.overlayPixel);
  desc.InputLayout = {elements, static_cast<UINT>(_countof(elements))};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = RENDER_TARGET_FORMAT;
  desc.DSVFormat = DEPTH_FORMAT;
  desc.SampleDesc.Count = m_sampleCount;
  desc.SampleMask = UINT_MAX;

  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  // A quad built from a vertex id has one winding and no back to cull; culling
  // nothing means its winding never has to agree with the meshes'.
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.FrontCounterClockwise = FALSE;
  desc.RasterizerState.DepthClipEnable = TRUE;

  D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
  blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  blend.BlendOp = D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
  blend.DestBlendAlpha = D3D12_BLEND_ONE;
  blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

  // Rings: tested, never written. Writing would make one ring occlude the next
  // where they overlap, which at fleet scale is most of them.
  desc.DepthStencilState.DepthEnable = TRUE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  // The ring lies exactly on the plane and so does nothing else, but a hull
  // resting on it is a coplanar surface away; ADR-006 §8 asks for a bias in the
  // MVP and this is it. Negative, so the ring wins a tie against what it lies on.
  desc.RasterizerState.DepthBias = -100;
  desc.RasterizerState.SlopeScaledDepthBias = -1.0f;
  desc.RasterizerState.DepthBiasClamp = 0.0f;
  check_hresult(_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_overlayRings.put())));
  NAME_D3D12_OBJECT(m_overlayRings);

  // Bars: no depth at all. This is the half of the rule a test cannot check.
  desc.DepthStencilState.DepthEnable = FALSE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.RasterizerState.DepthBias = 0;
  desc.RasterizerState.SlopeScaledDepthBias = 0.0f;
  check_hresult(_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_overlayBars.put())));
  NAME_D3D12_OBJECT(m_overlayBars);
  return true;
}

bool GpuPipelines::CreateUiPipeline(ID3D12Device* _device, const PipelineShaders& _shaders)
{
  if (_shaders.uiVertex.empty() || _shaders.uiPixel.empty())
  {
    NEURON_LOG_ERROR("the ui shaders are empty; the HUD would draw nothing");
    return false;
  }

  // The offsets are spelled by hand because D3D12 asks for them, which makes
  // them a second copy of UiInstance's layout -- and these are the asserts that
  // keep the copies honest. Same arrangement as the overlay's marks.
  static_assert(offsetof(UiInstance, rect) == 0, "UI_RECT is declared at offset 0");
  static_assert(offsetof(UiInstance, uv) == 16, "UI_UV at offset 16");
  static_assert(offsetof(UiInstance, colourRgba) == 32, "UI_COLOUR at offset 32");
  static_assert(offsetof(UiInstance, flags) == 36, "UI_FLAGS at offset 36");
  static_assert(offsetof(UiInstance, axis) == 40, "UI_AXIS at offset 40 -- appended, so nothing above it moved");

  // The colour is packed rather than four floats, the same as `OverlayMark`'s:
  // a HUD is thousands of glyph quads a frame and twelve bytes each of them
  // does not need is twelve bytes uploaded every frame.
  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"UI_RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"UI_UV", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"UI_COLOUR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"UI_FLAGS", 0, DXGI_FORMAT_R32_UINT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"UI_AXIS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = m_rootSignature.get();
  desc.InputLayout = {elements, static_cast<UINT>(_countof(elements))};
  desc.VS = {_shaders.uiVertex.data(), _shaders.uiVertex.size()};
  desc.PS = {_shaders.uiPixel.data(), _shaders.uiPixel.size()};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = RENDER_TARGET_FORMAT;
  desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
  // Always single-sampled, whatever the world passes use: the HUD draws after
  // the MSAA resolve, straight onto the back buffer (S14).
  desc.SampleDesc.Count = 1;
  desc.SampleMask = UINT_MAX;

  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.FrontCounterClockwise = FALSE;
  desc.RasterizerState.DepthClipEnable = FALSE;

  D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
  blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  blend.BlendOp = D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
  blend.DestBlendAlpha = D3D12_BLEND_ONE;
  blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

  /*
   * No depth at all, and no depth-stencil format either.
   *
   * The HUD is the last thing drawn and composites over everything, so a depth
   * test could only ever remove a pixel the player is meant to see. Declaring
   * `DSVFormat = UNKNOWN` rather than merely disabling the test is what says so
   * to the runtime: a pipeline with a format and no test still expects a bound
   * depth buffer, and the Ui pass deliberately does not bind one.
   */
  desc.DepthStencilState.DepthEnable = FALSE;
  desc.DepthStencilState.StencilEnable = FALSE;

  check_hresult(_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_ui.put())));
  NAME_D3D12_OBJECT(m_ui);

  /*
   * The same pipeline again, for the world layer.
   *
   * The ghost's lane is a screen-space quad like any other HUD element, but it
   * belongs *under* the hulls: an order passing behind a ship has to be behind
   * it. The only way to get that is to draw it before the Opaque pass, into the
   * world's own render target -- which is multisampled, and a PSO's sample count
   * must match the target it draws to. Hence a second one; everything else about
   * it is identical, and both come from the same `desc` for exactly that reason.
   *
   * There is no depth test here either. The lane does not need one: it is drawn
   * first and the ships are simply painted over it, which is occlusion by paint
   * order rather than by depth and is the whole trick. The *format* is declared
   * because the world passes leave a DSV bound, and a PSO claiming
   * `DXGI_FORMAT_UNKNOWN` against a bound DSV is a debug-layer error -- the same
   * note NebulaPass carries.
   */
  desc.SampleDesc.Count = m_sampleCount;
  desc.DSVFormat = DEPTH_FORMAT;
  check_hresult(_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_uiWorld.put())));
  NAME_D3D12_OBJECT(m_uiWorld);
  return true;
}

bool GpuPipelines::Create(ID3D12Device* _device, const PipelineShaders& _shaders, std::uint32_t _sampleCount)
{
  if (_device == nullptr || !AllPresent(_shaders))
  {
    return false;
  }

  Destroy();
  m_sampleCount = _sampleCount > 0 ? _sampleCount : 1;

  if (!CreateRootSignature(_device))
  {
    return false;
  }
  if (!CreateOpaquePipeline(_device, _shaders))
  {
    return false;
  }
  if (!CreateNebulaPipeline(_device, _shaders))
  {
    return false;
  }
  if (!CreateOverlayPipelines(_device, _shaders))
  {
    return false;
  }

  if (!CreateUiPipeline(_device, _shaders))
  {
    return false;
  }

  // The sizes, because they are the one thing about a compiled-in shader worth
  // seeing at boot: a stage that shrank to nothing between builds shows up here
  // rather than as an empty screen.
  NEURON_LOG_INFO("pipelines: opaque (%zu + %zu B), nebula (%zu + %zu B), overlay (%zu + %zu B), ui (%zu + %zu B) built",
                  _shaders.opaqueVertex.size(), _shaders.opaquePixel.size(), _shaders.nebulaVertex.size(),
                  _shaders.nebulaPixel.size(), _shaders.overlayVertex.size(), _shaders.overlayPixel.size(),
                  _shaders.uiVertex.size(), _shaders.uiPixel.size());
  return true;
}

void GpuPipelines::Destroy()
{
  // `m_ui` was missing here, which meant a rebuild -- the only caller is
  // `Create`, which destroys before it builds -- leaked the previous HUD
  // pipeline. Harmless in a run that creates its pipelines once and noticed
  // only when the world layer joined it, which is the usual way a leak in a
  // once-per-run path gets found.
  m_uiWorld = nullptr;
  m_ui = nullptr;
  m_overlayBars = nullptr;
  m_overlayRings = nullptr;
  m_nebula = nullptr;
  m_opaque = nullptr;
  m_rootSignature = nullptr;
}

} // namespace Neuron
