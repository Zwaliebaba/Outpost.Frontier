//! The root signature and the pipeline states of the fixed-function
//! shaders (record 0004, point 8): one root signature built in Rust, and a
//! pipeline state per [`PipelineKey`], made on first use and cached.
//!
//! The root signature has the draw's constant buffer as a root descriptor
//! (`b0`, both stages), a descriptor table of the two stage textures
//! (`t0`, `t1`, pixel stage) and a descriptor table of the 1,050 samplers
//! of `render::shader::samplers` (`s0` on, pixel stage). The samplers are
//! a fixed set made once in a sampler heap ([`sampler_descs`]), not static
//! samplers: Direct3D 12 does not bind a shader's sampler array to static
//! samplers ("sampler descriptor range is not fully bound").

use std::collections::HashMap;

use windows::Win32::Graphics::Direct3D::ID3DBlob;
use windows::Win32::Graphics::Direct3D12::{
    D3D_ROOT_SIGNATURE_VERSION_1, D3D12_BLEND, D3D12_BLEND_DESC, D3D12_BLEND_INV_SRC_ALPHA,
    D3D12_BLEND_ONE, D3D12_BLEND_OP, D3D12_BLEND_OP_ADD, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_ZERO,
    D3D12_COMPARISON_FUNC, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_COMPARISON_FUNC_NEVER,
    D3D12_CULL_MODE_BACK, D3D12_CULL_MODE_FRONT, D3D12_CULL_MODE_NONE, D3D12_DEPTH_STENCIL_DESC,
    D3D12_DEPTH_STENCILOP_DESC, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_DEPTH_WRITE_MASK_ZERO,
    D3D12_DESCRIPTOR_RANGE, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
    D3D12_FILL_MODE_SOLID, D3D12_FILL_MODE_WIREFRAME, D3D12_FILTER, D3D12_FILTER_ANISOTROPIC,
    D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FLOAT32_MAX, D3D12_GRAPHICS_PIPELINE_STATE_DESC,
    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, D3D12_INPUT_ELEMENT_DESC, D3D12_INPUT_LAYOUT_DESC,
    D3D12_LOGIC_OP_NOOP, D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
    D3D12_RASTERIZER_DESC, D3D12_RENDER_TARGET_BLEND_DESC, D3D12_ROOT_DESCRIPTOR,
    D3D12_ROOT_DESCRIPTOR_TABLE, D3D12_ROOT_PARAMETER, D3D12_ROOT_PARAMETER_0,
    D3D12_ROOT_PARAMETER_TYPE_CBV, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
    D3D12_ROOT_SIGNATURE_DESC, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
    D3D12_SAMPLER_DESC, D3D12_SHADER_BYTECODE, D3D12_SHADER_VISIBILITY_ALL,
    D3D12_SHADER_VISIBILITY_PIXEL, D3D12_STENCIL_OP_KEEP, D3D12_TEXTURE_ADDRESS_MODE,
    D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
    D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
    D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12SerializeRootSignature, ID3D12Device,
    ID3D12PipelineState, ID3D12RootSignature,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R32_FLOAT,
    DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
    DXGI_SAMPLE_DESC,
};
use windows::core::PCSTR;

use super::{Context, GpuError};
use crate::render::ff::draw::{PipelineKey, TopologyType};
use crate::render::ff::state::{Blend, Cull, Fill, RawValue};
use crate::render::shader::input::{self, Format};
use crate::render::shader::samplers::{self, Address, Border, FilterClass, Mip};

/// The render-target format of the scene.
pub const TARGET_FORMAT: DXGI_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM;
/// The depth-stencil format of the scene.
pub const DEPTH_FORMAT: DXGI_FORMAT = DXGI_FORMAT_D24_UNORM_S8_UINT;

/// Root parameter 0: the draw's constant buffer.
pub const ROOT_CONSTANTS: u32 = 0;
/// Root parameter 1: the stage textures' descriptor table.
pub const ROOT_TEXTURES: u32 = 1;
/// Root parameter 2: the samplers' descriptor table.
pub const ROOT_SAMPLERS: u32 = 2;

/// The compiled shaders (`cargo xtask shaders`).
pub struct Shaders {
    pub vs: &'static [u8],
    pub ps: &'static [u8],
    pub ps_flat: &'static [u8],
}

fn filter(class: FilterClass) -> (D3D12_FILTER, f32) {
    match class {
        FilterClass::Anisotropic { mipmaps } => (
            D3D12_FILTER_ANISOTROPIC,
            if mipmaps { D3D12_FLOAT32_MAX } else { 0.0 },
        ),
        FilterClass::MinMag {
            min_linear,
            mag_linear,
            mip,
        } => {
            // D3D12_FILTER encodes min in bits 4-5, mag in 2-3, mip in 0-1.
            let mip_linear = mip == Mip::Linear;
            let bits =
                (u32::from(min_linear) << 4) | (u32::from(mag_linear) << 2) | u32::from(mip_linear);
            let max_lod = if mip == Mip::None {
                0.0
            } else {
                D3D12_FLOAT32_MAX
            };
            (
                D3D12_FILTER(D3D12_FILTER_MIN_MAG_MIP_POINT.0 | bits.cast_signed()),
                max_lod,
            )
        }
    }
}

fn address(a: Address) -> D3D12_TEXTURE_ADDRESS_MODE {
    match a {
        Address::Wrap => D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        Address::Mirror => D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
        Address::Clamp => D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        Address::Border => D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        Address::MirrorOnce => D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
    }
}

fn border(b: Border) -> [f32; 4] {
    match b {
        Border::TransparentBlack => [0.0; 4],
        Border::OpaqueBlack => [0.0, 0.0, 0.0, 1.0],
        Border::OpaqueWhite => [1.0; 4],
    }
}

/// The fixed sampler set, in table order, for the sampler heap.
#[must_use]
pub fn sampler_descs() -> Vec<D3D12_SAMPLER_DESC> {
    (0..samplers::COUNT)
        .map(|i| {
            let desc = samplers::desc(i);
            let (filter, max_lod) = filter(desc.filter);
            D3D12_SAMPLER_DESC {
                Filter: filter,
                AddressU: address(desc.address_u),
                AddressV: address(desc.address_v),
                AddressW: D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                MipLODBias: 0.0,
                MaxAnisotropy: samplers::MAX_ANISOTROPY,
                ComparisonFunc: D3D12_COMPARISON_FUNC_NEVER,
                BorderColor: border(desc.border),
                MinLOD: 0.0,
                MaxLOD: max_lod,
            }
        })
        .collect()
}

/// The fixed-function root signature.
pub fn root_signature(device: &ID3D12Device) -> Result<ID3D12RootSignature, GpuError> {
    let range = D3D12_DESCRIPTOR_RANGE {
        RangeType: D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        NumDescriptors: 2,
        BaseShaderRegister: 0,
        RegisterSpace: 0,
        OffsetInDescriptorsFromTableStart: 0,
    };
    let sampler_range = D3D12_DESCRIPTOR_RANGE {
        RangeType: D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
        NumDescriptors: samplers::COUNT,
        BaseShaderRegister: 0,
        RegisterSpace: 0,
        OffsetInDescriptorsFromTableStart: 0,
    };
    let parameters = [
        D3D12_ROOT_PARAMETER {
            ParameterType: D3D12_ROOT_PARAMETER_TYPE_CBV,
            Anonymous: D3D12_ROOT_PARAMETER_0 {
                Descriptor: D3D12_ROOT_DESCRIPTOR {
                    ShaderRegister: 0,
                    RegisterSpace: 0,
                },
            },
            ShaderVisibility: D3D12_SHADER_VISIBILITY_ALL,
        },
        D3D12_ROOT_PARAMETER {
            ParameterType: D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            Anonymous: D3D12_ROOT_PARAMETER_0 {
                DescriptorTable: D3D12_ROOT_DESCRIPTOR_TABLE {
                    NumDescriptorRanges: 1,
                    pDescriptorRanges: &range,
                },
            },
            ShaderVisibility: D3D12_SHADER_VISIBILITY_PIXEL,
        },
        D3D12_ROOT_PARAMETER {
            ParameterType: D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            Anonymous: D3D12_ROOT_PARAMETER_0 {
                DescriptorTable: D3D12_ROOT_DESCRIPTOR_TABLE {
                    NumDescriptorRanges: 1,
                    pDescriptorRanges: &sampler_range,
                },
            },
            ShaderVisibility: D3D12_SHADER_VISIBILITY_PIXEL,
        },
    ];
    let desc = D3D12_ROOT_SIGNATURE_DESC {
        NumParameters: parameters.len() as u32,
        pParameters: parameters.as_ptr(),
        NumStaticSamplers: 0,
        pStaticSamplers: std::ptr::null(),
        Flags: D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
    };
    let mut blob: Option<ID3DBlob> = None;
    let mut error: Option<ID3DBlob> = None;
    // SAFETY: `desc` points at `parameters` and the ranges, which
    // live until the call returns; the out slots are valid.
    unsafe {
        D3D12SerializeRootSignature(
            &desc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &mut blob,
            Some(&mut error),
        )
    }
    .at("D3D12SerializeRootSignature")?;
    let blob = blob.ok_or(GpuError {
        call: "D3D12SerializeRootSignature",
        code: 0,
    })?;
    // SAFETY: the blob's pointer and size describe its own buffer, which
    // lives as long as `blob`.
    let bytes = unsafe {
        std::slice::from_raw_parts(blob.GetBufferPointer().cast::<u8>(), blob.GetBufferSize())
    };
    // SAFETY: a serialised root signature for node 0.
    unsafe { device.CreateRootSignature(0, bytes) }.at("CreateRootSignature")
}

fn blend(b: Blend) -> D3D12_BLEND {
    // D3DBLEND 1 to 11 have D3D12_BLEND's numbers; the two BOTH modes are
    // resolved by the caller.
    D3D12_BLEND(b.to_raw().clamp(1, 11).cast_signed())
}

/// The blend factor for the alpha channel: Direct3D 12 refuses colour
/// factors there, which on alpha mean the alpha factor.
fn alpha_blend(b: D3D12_BLEND) -> D3D12_BLEND {
    match b.0 {
        3 => D3D12_BLEND(5),
        4 => D3D12_BLEND(6),
        9 => D3D12_BLEND(7),
        10 => D3D12_BLEND(8),
        _ => b,
    }
}

fn blend_desc(key: &PipelineKey) -> D3D12_BLEND_DESC {
    let mut target = D3D12_RENDER_TARGET_BLEND_DESC {
        BlendEnable: false.into(),
        LogicOpEnable: false.into(),
        SrcBlend: D3D12_BLEND_ONE,
        DestBlend: D3D12_BLEND_ZERO,
        BlendOp: D3D12_BLEND_OP_ADD,
        SrcBlendAlpha: D3D12_BLEND_ONE,
        DestBlendAlpha: D3D12_BLEND_ZERO,
        BlendOpAlpha: D3D12_BLEND_OP_ADD,
        LogicOp: D3D12_LOGIC_OP_NOOP,
        RenderTargetWriteMask: key.color_write,
    };
    if let Some((src, dst, op)) = key.blend {
        let (s, d) = match src {
            Blend::BothSrcAlpha => (D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA),
            Blend::BothInvSrcAlpha => (D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_SRC_ALPHA),
            _ => (blend(src), blend(dst)),
        };
        let op = D3D12_BLEND_OP(op.to_raw().clamp(1, 5).cast_signed());
        target.BlendEnable = true.into();
        target.SrcBlend = s;
        target.DestBlend = d;
        target.BlendOp = op;
        target.SrcBlendAlpha = alpha_blend(s);
        target.DestBlendAlpha = alpha_blend(d);
        target.BlendOpAlpha = op;
    }
    let mut desc = D3D12_BLEND_DESC::default();
    desc.RenderTarget[0] = target;
    desc
}

fn rasterizer_desc(key: &PipelineKey) -> D3D12_RASTERIZER_DESC {
    D3D12_RASTERIZER_DESC {
        // Point fill has no Direct3D 12 equivalent: drawn as wireframe.
        FillMode: match key.fill {
            Fill::Point | Fill::Wireframe => D3D12_FILL_MODE_WIREFRAME,
            _ => D3D12_FILL_MODE_SOLID,
        },
        // Clockwise triangles are front faces, as in Direct3D 8: CULL_CCW
        // (the default) culls the back faces.
        CullMode: match key.cull {
            Cull::Clockwise => D3D12_CULL_MODE_FRONT,
            Cull::CounterClockwise => D3D12_CULL_MODE_BACK,
            _ => D3D12_CULL_MODE_NONE,
        },
        FrontCounterClockwise: false.into(),
        // ZBIAS moves towards the viewer, one unit per step.
        DepthBias: -key.depth_bias,
        DepthBiasClamp: 0.0,
        SlopeScaledDepthBias: 0.0,
        DepthClipEnable: true.into(),
        ..Default::default()
    }
}

/// Direct3D's default stencil face: keep on every outcome, always pass.
const KEEP_ALWAYS: D3D12_DEPTH_STENCILOP_DESC = D3D12_DEPTH_STENCILOP_DESC {
    StencilFailOp: D3D12_STENCIL_OP_KEEP,
    StencilDepthFailOp: D3D12_STENCIL_OP_KEEP,
    StencilPassOp: D3D12_STENCIL_OP_KEEP,
    StencilFunc: D3D12_COMPARISON_FUNC_ALWAYS,
};

fn depth_desc(key: &PipelineKey) -> D3D12_DEPTH_STENCIL_DESC {
    let mut desc = D3D12_DEPTH_STENCIL_DESC {
        DepthEnable: key.depth.is_some().into(),
        DepthWriteMask: D3D12_DEPTH_WRITE_MASK_ZERO,
        DepthFunc: D3D12_COMPARISON_FUNC_ALWAYS,
        // Stencil is off, but Direct3D validates the operations anyway:
        // its defaults.
        StencilEnable: false.into(),
        StencilReadMask: 0xff,
        StencilWriteMask: 0xff,
        FrontFace: KEEP_ALWAYS,
        BackFace: KEEP_ALWAYS,
    };
    if let Some((func, write)) = key.depth {
        desc.DepthFunc = D3D12_COMPARISON_FUNC(func.to_raw().clamp(1, 8).cast_signed());
        if write {
            desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        }
    }
    desc
}

fn topology(t: TopologyType) -> D3D12_PRIMITIVE_TOPOLOGY_TYPE {
    match t {
        TopologyType::Point => D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT,
        TopologyType::Line => D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,
        TopologyType::Triangle => D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
    }
}

fn format(f: Format) -> DXGI_FORMAT {
    match f {
        Format::R32Float => DXGI_FORMAT_R32_FLOAT,
        Format::R32G32Float => DXGI_FORMAT_R32G32_FLOAT,
        Format::R32G32B32Float => DXGI_FORMAT_R32G32B32_FLOAT,
        Format::R32G32B32A32Float => DXGI_FORMAT_R32G32B32A32_FLOAT,
        Format::B8G8R8A8Unorm => DXGI_FORMAT_B8G8R8A8_UNORM,
    }
}

/// A semantic name as the NUL-terminated string the input layout points
/// at.
fn semantic(name: &str) -> PCSTR {
    let bytes: &'static [u8] = match name {
        "POSITION" => b"POSITION\0",
        "BLENDWEIGHT" => b"BLENDWEIGHT\0",
        "NORMAL" => b"NORMAL\0",
        "COLOR" => b"COLOR\0",
        _ => b"TEXCOORD\0",
    };
    PCSTR(bytes.as_ptr())
}

fn bytecode(blob: &[u8]) -> D3D12_SHADER_BYTECODE {
    D3D12_SHADER_BYTECODE {
        pShaderBytecode: blob.as_ptr().cast(),
        BytecodeLength: blob.len(),
    }
}

/// The pipeline states made so far, by key.
pub struct PipelineCache {
    root: ID3D12RootSignature,
    shaders: Shaders,
    states: HashMap<PipelineKey, ID3D12PipelineState>,
}

impl PipelineCache {
    pub fn new(device: &ID3D12Device, shaders: Shaders) -> Result<PipelineCache, GpuError> {
        Ok(PipelineCache {
            root: root_signature(device)?,
            shaders,
            states: HashMap::new(),
        })
    }

    #[must_use]
    pub fn root_signature(&self) -> &ID3D12RootSignature {
        &self.root
    }

    /// The number of pipeline states made.
    #[must_use]
    pub fn len(&self) -> usize {
        self.states.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.states.is_empty()
    }

    /// The pipeline state for `key`, made on first use.
    pub fn get(
        &mut self,
        device: &ID3D12Device,
        key: &PipelineKey,
    ) -> Result<&ID3D12PipelineState, GpuError> {
        if !self.states.contains_key(key) {
            let state = self.create(device, key)?;
            self.states.insert(*key, state);
        }
        Ok(&self.states[key])
    }

    fn create(
        &self,
        device: &ID3D12Device,
        key: &PipelineKey,
    ) -> Result<ID3D12PipelineState, GpuError> {
        let elements: Vec<D3D12_INPUT_ELEMENT_DESC> = input::layout(key.fvf)
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
            pRootSignature: std::mem::ManuallyDrop::new(Some(self.root.clone())),
            VS: bytecode(self.shaders.vs),
            PS: bytecode(if key.flat {
                self.shaders.ps_flat
            } else {
                self.shaders.ps
            }),
            BlendState: blend_desc(key),
            SampleMask: u32::MAX,
            RasterizerState: rasterizer_desc(key),
            DepthStencilState: depth_desc(key),
            InputLayout: D3D12_INPUT_LAYOUT_DESC {
                pInputElementDescs: elements.as_ptr(),
                NumElements: elements.len() as u32,
            },
            PrimitiveTopologyType: topology(key.topology),
            NumRenderTargets: 1,
            DSVFormat: DEPTH_FORMAT,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            ..Default::default()
        };
        desc.RTVFormats[0] = TARGET_FORMAT;
        // SAFETY: the description points at `elements`, the static shader
        // blobs and the semantic names, all live for the call.
        let state =
            unsafe { device.CreateGraphicsPipelineState(&desc) }.at("CreateGraphicsPipelineState");
        // SAFETY: the root-signature reference the description holds is
        // dropped once.
        unsafe { std::mem::ManuallyDrop::drop(&mut desc.pRootSignature) };
        state
    }
}

/// The defaults vertex of slot 1 (`render::shader::input::DEFAULTS`).
#[must_use]
pub fn defaults_vertex() -> &'static [u8] {
    &input::DEFAULTS
}
