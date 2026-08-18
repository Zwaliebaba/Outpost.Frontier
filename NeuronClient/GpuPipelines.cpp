#include "pch.h"

#include "GpuPipelines.h"

#include "Log.h"
#include "RenderWorld.h"

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
                  !_shaders.nebulaPixel.empty();
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
  static_assert(sizeof(MeshVertex) == 24 && offsetof(MeshVertex, normal) == 12, "NORMAL is declared at offset 12");

  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"INSTANCE_POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"INSTANCE_HEADING", 0, DXGI_FORMAT_R32_FLOAT, 1, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"INSTANCE_CHANNELS", 0, DXGI_FORMAT_R8G8_UINT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
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
  desc.SampleDesc.Count = 1; // No MSAA yet; the 4x offscreen target is S14.
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
  desc.SampleDesc.Count = 1;
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

bool GpuPipelines::Create(ID3D12Device* _device, const PipelineShaders& _shaders)
{
  if (_device == nullptr || !AllPresent(_shaders))
  {
    return false;
  }

  Destroy();

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

  // The sizes, because they are the one thing about a compiled-in shader worth
  // seeing at boot: a stage that shrank to nothing between builds shows up here
  // rather than as an empty screen.
  NEURON_LOG_INFO("pipelines: opaque (%zu + %zu B) and nebula (%zu + %zu B) built", _shaders.opaqueVertex.size(),
                  _shaders.opaquePixel.size(), _shaders.nebulaVertex.size(), _shaders.nebulaPixel.size());
  return true;
}

void GpuPipelines::Destroy()
{
  m_nebula = nullptr;
  m_opaque = nullptr;
  m_rootSignature = nullptr;
}

} // namespace Neuron
