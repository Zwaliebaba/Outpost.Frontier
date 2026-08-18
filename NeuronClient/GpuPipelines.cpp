#include "pch.h"

#include "GpuPipelines.h"

#include "Log.h"
#include "RenderWorld.h"

#include <d3dcompiler.h>

#include <cstddef>
#include <string>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace Neuron
{
namespace
{

std::string JoinPath(std::string_view _directory, std::string_view _fileName)
{
  std::string path(_directory);
  if (!path.empty() && path.back() != '/' && path.back() != '\\')
  {
    path += '/';
  }
  path.append(_fileName);
  return path;
}

std::wstring ToWide(const std::string& _utf8)
{
  const int count = MultiByteToWideChar(CP_UTF8, 0, _utf8.c_str(), -1, nullptr, 0);
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, _utf8.c_str(), -1, result.data(), count);
  return result;
}

/*
 * Compiles one entry point, from a file, at boot.
 *
 * D3DCompileFromFile rather than a build step, and shader model 5.1 rather than
 * 6.x: 5.1 is everything the MVP frame asks for, d3dcompiler_47.dll ships with
 * Windows, and dxcompiler.dll does not. The trade is that a broken shader is
 * found when the client starts rather than when it builds -- which is why the
 * failure path prints the compiler's own message, file and line included, and
 * refuses to start rather than rendering nothing and saying nothing.
 */
[[nodiscard]] bool CompileShader(const std::string& _path, const char* _entryPoint, const char* _target, GpuPtr<ID3DBlob>& _outBlob)
{
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#if defined(_DEBUG)
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
  flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

  GpuPtr<ID3DBlob> errors;
  const HRESULT result = D3DCompileFromFile(ToWide(_path).c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, _entryPoint, _target,
                                            flags, 0, _outBlob.put(), errors.put());
  if (FAILED(result))
  {
    if (errors != nullptr)
    {
      NEURON_LOG_ERROR("%s", static_cast<const char*>(errors->GetBufferPointer()));
    }
    NEURON_LOG_ERROR("could not compile %s:%s as %s (hr 0x%08lx)", _path.c_str(), _entryPoint, _target,
                     static_cast<unsigned long>(result));
    return false;
  }

  // A warning-free compile is the bar; warnings are errors above, so anything
  // that reaches here and still has a message is worth reading.
  if (errors != nullptr && errors->GetBufferSize() > 1)
  {
    NEURON_LOG_WARNING("%s", static_cast<const char*>(errors->GetBufferPointer()));
  }
  return true;
}

} // namespace

GpuPipelines::~GpuPipelines()
{
  Destroy();
}

bool GpuPipelines::CreateRootSignature(ID3D12Device* _device)
{
  // One SRV table, holding the glyph atlas today and whatever the Ui and
  // OverlayWorld passes sample tomorrow. DATA_STATIC is the honest flag here:
  // the atlas texture and its descriptor are both written once at boot and
  // never touched again, which is exactly what the flag promises the driver.
  D3D12_DESCRIPTOR_RANGE1 textureRange{};
  textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  textureRange.NumDescriptors = 1;
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

  // One static sampler: the glyph atlas wants linear filtering and clamping,
  // and a static sampler costs no descriptor heap space at all.
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = static_cast<UINT>(_countof(parameters));
  desc.Desc_1_1.pParameters = parameters;
  desc.Desc_1_1.NumStaticSamplers = 1;
  desc.Desc_1_1.pStaticSamplers = &sampler;
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

bool GpuPipelines::CreateOpaquePipeline(ID3D12Device* _device, std::string_view _shaderDirectory)
{
  const std::string path = JoinPath(_shaderDirectory, "Opaque.hlsl");

  GpuPtr<ID3DBlob> vertexShader;
  GpuPtr<ID3DBlob> pixelShader;
  if (!CompileShader(path, "VertexMain", "vs_5_1", vertexShader) || !CompileShader(path, "PixelMain", "ps_5_1", pixelShader))
  {
    return false;
  }

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
  desc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
  desc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
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

bool GpuPipelines::Create(ID3D12Device* _device, std::string_view _shaderDirectory)
{
  if (_device == nullptr)
  {
    return false;
  }

  Destroy();

  if (!CreateRootSignature(_device))
  {
    return false;
  }
  if (!CreateOpaquePipeline(_device, _shaderDirectory))
  {
    return false;
  }

  NEURON_LOG_INFO("pipelines: opaque built from %s", std::string(_shaderDirectory).c_str());
  return true;
}

void GpuPipelines::Destroy()
{
  m_opaque = nullptr;
  m_rootSignature = nullptr;
}

} // namespace Neuron
