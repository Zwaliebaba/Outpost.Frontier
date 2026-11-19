//! The Direct3D 12 objects of the programmable passes (record 0005,
//! points 7 and 8): the pass root signature, a pipeline state per
//! [`ExPipelineKey`], the sampler of each [`SamplerState`] and the formats
//! and views of Ex textures.
//!
//! The pass root signature is the programs' own, beside the fixed-function
//! one of record 0004: the vertex registers as a root constant buffer at
//! `b0` seen by the vertex stage, the pixel registers as one at `b0` seen
//! by the pixel stage (each stage has its own `c` registers in Direct3D 9),
//! a table of sixteen views `t0` to `t15` and a table of sixteen samplers
//! `s0` to `s15`, both for the pixel stage. The tables are copied per draw
//! into the same shader-visible rings the fixed-function draws use.

use windows::Win32::Graphics::Direct3D12::{
    D3D12_COMPARISON_FUNC_NEVER, D3D12_DESCRIPTOR_RANGE, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
    D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_FILTER, D3D12_FILTER_MIN_MAG_MIP_POINT,
    D3D12_FLOAT32_MAX, D3D12_GRAPHICS_PIPELINE_STATE_DESC,
    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, D3D12_INPUT_ELEMENT_DESC, D3D12_INPUT_LAYOUT_DESC,
    D3D12_ROOT_DESCRIPTOR, D3D12_ROOT_DESCRIPTOR_TABLE, D3D12_ROOT_PARAMETER,
    D3D12_ROOT_PARAMETER_0, D3D12_ROOT_PARAMETER_TYPE_CBV,
    D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, D3D12_ROOT_SIGNATURE_DESC,
    D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, D3D12_SAMPLER_DESC,
    D3D12_SHADER_RESOURCE_VIEW_DESC, D3D12_SHADER_RESOURCE_VIEW_DESC_0,
    D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SRV_DIMENSION_TEXTURE2D,
    D3D12_SRV_DIMENSION_TEXTURE3D, D3D12_TEX2D_SRV, D3D12_TEX3D_SRV, D3D12_TEXTURE_ADDRESS_MODE,
    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
    D3D12_TEXTURE_ADDRESS_MODE_WRAP, ID3D12Device, ID3D12PipelineState, ID3D12RootSignature,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16_FLOAT,
    DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
    DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC,
};

use super::pipeline::{
    DEPTH_FORMAT, blend_desc, bytecode, create_root_signature, depth_desc, format, rasterizer_desc,
    semantic, topology,
};
use super::{Context, GpuError};
use crate::render::pass::{
    Address, ExFormat, ExPipelineKey, Filter, MipFilter, SamplerState, TEXTURE_SLOTS,
};
use crate::render::shader::input;

/// Root parameter 0: the vertex registers (`b0`, vertex stage).
pub const ROOT_VERTEX_CONSTANTS: u32 = 0;
/// Root parameter 1: the pixel registers (`b0`, pixel stage).
pub const ROOT_PIXEL_CONSTANTS: u32 = 1;
/// Root parameter 2: the views `t0` to `t15`.
pub const ROOT_VIEWS: u32 = 2;
/// Root parameter 3: the samplers `s0` to `s15`.
pub const ROOT_SAMPLERS: u32 = 3;

/// `D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING`: each output channel takes a
/// source channel (0 to 3), 0 (4) or 1 (5).
#[must_use]
pub const fn component_mapping(r: u32, g: u32, b: u32, a: u32) -> u32 {
    r | (g << 3) | (b << 6) | (a << 9) | (1 << 12)
}

/// Source channel "always 1" of [`component_mapping`].
const ONE: u32 = 5;

/// The resource format of an Ex texture.
#[must_use]
pub fn dxgi_format(f: ExFormat) -> DXGI_FORMAT {
    match f {
        ExFormat::Bgra8 => DXGI_FORMAT_B8G8R8A8_UNORM,
        ExFormat::L8 => DXGI_FORMAT_R8_UNORM,
        ExFormat::Rgba16Float => DXGI_FORMAT_R16G16B16A16_FLOAT,
        ExFormat::R16Float => DXGI_FORMAT_R16_FLOAT,
        ExFormat::R32Float => DXGI_FORMAT_R32_FLOAT,
        ExFormat::Rg16Float => DXGI_FORMAT_R16G16_FLOAT,
    }
}

/// How a view of `f` reads, as Direct3D 9 sampled the format: luminance in
/// red, green and blue with alpha 1; channels a float format lacks read 1.
#[must_use]
pub fn sampled_mapping(f: ExFormat) -> u32 {
    match f {
        ExFormat::Bgra8 | ExFormat::Rgba16Float => component_mapping(0, 1, 2, 3),
        ExFormat::L8 => component_mapping(0, 0, 0, ONE),
        ExFormat::R16Float | ExFormat::R32Float => component_mapping(0, ONE, ONE, ONE),
        ExFormat::Rg16Float => component_mapping(0, 1, ONE, ONE),
    }
}

/// The scene depth reads its depth in every channel (the C++ promised it
/// in red).
pub const DEPTH_MAPPING: u32 = component_mapping(0, 0, 0, 0);

/// A view of level 0 of a 2D texture.
#[must_use]
pub fn view_2d(format: DXGI_FORMAT, mapping: u32) -> D3D12_SHADER_RESOURCE_VIEW_DESC {
    D3D12_SHADER_RESOURCE_VIEW_DESC {
        Format: format,
        ViewDimension: D3D12_SRV_DIMENSION_TEXTURE2D,
        Shader4ComponentMapping: mapping,
        Anonymous: D3D12_SHADER_RESOURCE_VIEW_DESC_0 {
            Texture2D: D3D12_TEX2D_SRV {
                MostDetailedMip: 0,
                MipLevels: 1,
                PlaneSlice: 0,
                ResourceMinLODClamp: 0.0,
            },
        },
    }
}

/// A view of the one level of a volume texture.
#[must_use]
pub fn view_3d(format: DXGI_FORMAT, mapping: u32) -> D3D12_SHADER_RESOURCE_VIEW_DESC {
    D3D12_SHADER_RESOURCE_VIEW_DESC {
        Format: format,
        ViewDimension: D3D12_SRV_DIMENSION_TEXTURE3D,
        Shader4ComponentMapping: mapping,
        Anonymous: D3D12_SHADER_RESOURCE_VIEW_DESC_0 {
            Texture3D: D3D12_TEX3D_SRV {
                MostDetailedMip: 0,
                MipLevels: 1,
                ResourceMinLODClamp: 0.0,
            },
        },
    }
}

fn address(a: Address) -> D3D12_TEXTURE_ADDRESS_MODE {
    match a {
        Address::Wrap => D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        Address::Clamp => D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        Address::Mirror => D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
    }
}

/// The sampler of `s`: the same filter for minification and magnification;
/// without a mip filter only level 0 is read.
#[must_use]
pub fn sampler_desc(s: SamplerState) -> D3D12_SAMPLER_DESC {
    let linear = u32::from(s.filter == Filter::Linear);
    // D3D12_FILTER encodes min in bits 4-5, mag in 2-3, mip in 0-1.
    let mip = u32::from(s.mip == MipFilter::Linear);
    let bits = (linear << 4) | (linear << 2) | mip;
    D3D12_SAMPLER_DESC {
        Filter: D3D12_FILTER(D3D12_FILTER_MIN_MAG_MIP_POINT.0 | bits.cast_signed()),
        AddressU: address(s.address[0]),
        AddressV: address(s.address[1]),
        AddressW: address(s.address[2]),
        MipLODBias: 0.0,
        MaxAnisotropy: 1,
        ComparisonFunc: D3D12_COMPARISON_FUNC_NEVER,
        BorderColor: [0.0; 4],
        MinLOD: 0.0,
        MaxLOD: if s.mip == MipFilter::None {
            0.0
        } else {
            D3D12_FLOAT32_MAX
        },
    }
}

/// The pass root signature.
pub fn root_signature(device: &ID3D12Device) -> Result<ID3D12RootSignature, GpuError> {
    let views = D3D12_DESCRIPTOR_RANGE {
        RangeType: D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        NumDescriptors: TEXTURE_SLOTS as u32,
        BaseShaderRegister: 0,
        RegisterSpace: 0,
        OffsetInDescriptorsFromTableStart: 0,
    };
    let samplers = D3D12_DESCRIPTOR_RANGE {
        RangeType: D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
        ..views
    };
    let cbv = |visibility| D3D12_ROOT_PARAMETER {
        ParameterType: D3D12_ROOT_PARAMETER_TYPE_CBV,
        Anonymous: D3D12_ROOT_PARAMETER_0 {
            Descriptor: D3D12_ROOT_DESCRIPTOR {
                ShaderRegister: 0,
                RegisterSpace: 0,
            },
        },
        ShaderVisibility: visibility,
    };
    let table = |range: &D3D12_DESCRIPTOR_RANGE| D3D12_ROOT_PARAMETER {
        ParameterType: D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
        Anonymous: D3D12_ROOT_PARAMETER_0 {
            DescriptorTable: D3D12_ROOT_DESCRIPTOR_TABLE {
                NumDescriptorRanges: 1,
                pDescriptorRanges: std::ptr::from_ref(range),
            },
        },
        ShaderVisibility: D3D12_SHADER_VISIBILITY_PIXEL,
    };
    let parameters = [
        cbv(D3D12_SHADER_VISIBILITY_VERTEX),
        cbv(D3D12_SHADER_VISIBILITY_PIXEL),
        table(&views),
        table(&samplers),
    ];
    let desc = D3D12_ROOT_SIGNATURE_DESC {
        NumParameters: parameters.len() as u32,
        pParameters: parameters.as_ptr(),
        NumStaticSamplers: 0,
        pStaticSamplers: std::ptr::null(),
        Flags: D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
    };
    create_root_signature(device, &desc)
}

/// The pipeline state of a programmable draw: the program's blobs (DXIL)
/// with the key's states, the vertex format's input layout (the missing
/// elements from the defaults vertex in slot 1, as for the fixed-function
/// draws), the target's format and the depth format when depth is bound.
pub fn pipeline_state(
    device: &ID3D12Device,
    root: &ID3D12RootSignature,
    key: &ExPipelineKey,
    vs: &[u8],
    ps: &[u8],
) -> Result<ID3D12PipelineState, GpuError> {
    let elements: Vec<D3D12_INPUT_ELEMENT_DESC> = input::layout(key.base.fvf)
        .iter()
        .map(|e| D3D12_INPUT_ELEMENT_DESC {
            SemanticName: semantic(e.semantic),
            SemanticIndex: e.index,
            Format: format(e.format),
            InputSlot: e.slot,
            AlignedByteOffset: e.offset,
            InputSlotClass: D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            InstanceDataStepRate: 0,
        })
        .collect();
    let mut desc = D3D12_GRAPHICS_PIPELINE_STATE_DESC {
        pRootSignature: std::mem::ManuallyDrop::new(Some(root.clone())),
        VS: bytecode(vs),
        PS: bytecode(ps),
        BlendState: blend_desc(&key.base),
        SampleMask: u32::MAX,
        RasterizerState: rasterizer_desc(&key.base),
        DepthStencilState: depth_desc(&key.base),
        InputLayout: D3D12_INPUT_LAYOUT_DESC {
            pInputElementDescs: elements.as_ptr(),
            NumElements: elements.len() as u32,
        },
        PrimitiveTopologyType: topology(key.base.topology),
        NumRenderTargets: 1,
        DSVFormat: if key.depth_bound {
            DEPTH_FORMAT
        } else {
            DXGI_FORMAT_UNKNOWN
        },
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        ..Default::default()
    };
    desc.RTVFormats[0] = dxgi_format(key.target_format);
    // SAFETY: the description points at `elements`, the program's blobs
    // and the static semantic names, all live for the call.
    let state =
        unsafe { device.CreateGraphicsPipelineState(&desc) }.at("CreateGraphicsPipelineState");
    // SAFETY: the root-signature reference the description holds is dropped
    // once.
    unsafe { std::mem::ManuallyDrop::drop(&mut desc.pRootSignature) };
    state
}
