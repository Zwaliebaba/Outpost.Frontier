//! Presenting (record 0004, point 8): the final blit of the scene target
//! through the gamma ramp, and the flip-model swap chain it blits into.
//!
//! The blit has its own root signature (one table of two views: the scene
//! and the ramp texture) and pipeline state (no vertex input, no depth),
//! and the shaders of `shaders/blit.hlsl`. The ramp is a 256 by 1
//! `R16G16B16A16_UNORM` texture refreshed when the device's ramp changes.

use windows::Win32::Foundation::HWND;
use windows::Win32::Graphics::Direct3D12::{
    D3D12_BLEND_DESC, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_BLEND_ZERO,
    D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_CULL_MODE_NONE,
    D3D12_DEPTH_STENCIL_DESC, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_DESCRIPTOR_RANGE,
    D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_FILL_MODE_SOLID, D3D12_GRAPHICS_PIPELINE_STATE_DESC,
    D3D12_LOGIC_OP_NOOP, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D12_RASTERIZER_DESC,
    D3D12_RENDER_TARGET_BLEND_DESC, D3D12_ROOT_DESCRIPTOR_TABLE, D3D12_ROOT_PARAMETER,
    D3D12_ROOT_PARAMETER_0, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, D3D12_ROOT_SIGNATURE_DESC,
    D3D12_ROOT_SIGNATURE_FLAG_NONE, D3D12_SHADER_VISIBILITY_PIXEL, ID3D12Device,
    ID3D12PipelineState, ID3D12Resource, ID3D12RootSignature,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_SAMPLE_DESC,
};
use windows::Win32::Graphics::Dxgi::{
    CreateDXGIFactory2, DXGI_CREATE_FACTORY_FLAGS, DXGI_PRESENT, DXGI_SCALING_STRETCH,
    DXGI_SWAP_CHAIN_DESC1, DXGI_SWAP_CHAIN_FLAG, DXGI_SWAP_EFFECT_FLIP_DISCARD,
    DXGI_USAGE_RENDER_TARGET_OUTPUT, IDXGIFactory4, IDXGISwapChain1, IDXGISwapChain3,
};
use windows::core::Interface;

use super::pipeline::{KEEP_ALWAYS, TARGET_FORMAT, bytecode, create_root_signature};
use super::{Context, Gpu, GpuError};

/// The compiled blit shaders.
pub const BLIT_VS: &[u8] = include_bytes!("../../../compiled_shaders/blit_vs.cso");
pub const BLIT_PS: &[u8] = include_bytes!("../../../compiled_shaders/blit_ps.cso");

/// The ramp texture's bytes: 256 texels of red, green, blue and a full
/// alpha, UNORM16 little-endian.
#[must_use]
pub fn ramp_texels(ramp: &crate::render::ff::gamma::GammaRamp) -> Vec<u8> {
    let mut out = Vec::with_capacity(256 * 8);
    for i in 0..256 {
        for word in [ramp.red[i], ramp.green[i], ramp.blue[i], u16::MAX] {
            out.extend_from_slice(&word.to_le_bytes());
        }
    }
    out
}

/// The blit's root signature and pipeline state.
pub struct Blit {
    pub root: ID3D12RootSignature,
    pub state: ID3D12PipelineState,
}

impl Blit {
    pub fn new(device: &ID3D12Device) -> Result<Blit, GpuError> {
        let range = D3D12_DESCRIPTOR_RANGE {
            RangeType: D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            NumDescriptors: 2,
            BaseShaderRegister: 0,
            RegisterSpace: 0,
            OffsetInDescriptorsFromTableStart: 0,
        };
        let parameters = [D3D12_ROOT_PARAMETER {
            ParameterType: D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            Anonymous: D3D12_ROOT_PARAMETER_0 {
                DescriptorTable: D3D12_ROOT_DESCRIPTOR_TABLE {
                    NumDescriptorRanges: 1,
                    pDescriptorRanges: &range,
                },
            },
            ShaderVisibility: D3D12_SHADER_VISIBILITY_PIXEL,
        }];
        let root = create_root_signature(
            device,
            &D3D12_ROOT_SIGNATURE_DESC {
                NumParameters: 1,
                pParameters: parameters.as_ptr(),
                NumStaticSamplers: 0,
                pStaticSamplers: std::ptr::null(),
                Flags: D3D12_ROOT_SIGNATURE_FLAG_NONE,
            },
        )?;
        let mut blend = D3D12_BLEND_DESC::default();
        blend.RenderTarget[0] = D3D12_RENDER_TARGET_BLEND_DESC {
            BlendEnable: false.into(),
            LogicOpEnable: false.into(),
            SrcBlend: D3D12_BLEND_ONE,
            DestBlend: D3D12_BLEND_ZERO,
            BlendOp: D3D12_BLEND_OP_ADD,
            SrcBlendAlpha: D3D12_BLEND_ONE,
            DestBlendAlpha: D3D12_BLEND_ZERO,
            BlendOpAlpha: D3D12_BLEND_OP_ADD,
            LogicOp: D3D12_LOGIC_OP_NOOP,
            RenderTargetWriteMask: D3D12_COLOR_WRITE_ENABLE_ALL.0 as u8,
        };
        let mut desc = D3D12_GRAPHICS_PIPELINE_STATE_DESC {
            pRootSignature: std::mem::ManuallyDrop::new(Some(root.clone())),
            VS: bytecode(BLIT_VS),
            PS: bytecode(BLIT_PS),
            BlendState: blend,
            SampleMask: u32::MAX,
            RasterizerState: D3D12_RASTERIZER_DESC {
                FillMode: D3D12_FILL_MODE_SOLID,
                CullMode: D3D12_CULL_MODE_NONE,
                DepthClipEnable: true.into(),
                ..Default::default()
            },
            DepthStencilState: D3D12_DEPTH_STENCIL_DESC {
                DepthEnable: false.into(),
                DepthWriteMask: D3D12_DEPTH_WRITE_MASK_ZERO,
                DepthFunc: D3D12_COMPARISON_FUNC_ALWAYS,
                StencilEnable: false.into(),
                StencilReadMask: 0xff,
                StencilWriteMask: 0xff,
                FrontFace: KEEP_ALWAYS,
                BackFace: KEEP_ALWAYS,
            },
            PrimitiveTopologyType: D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
            NumRenderTargets: 1,
            DSVFormat: DXGI_FORMAT_UNKNOWN,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            ..Default::default()
        };
        desc.RTVFormats[0] = TARGET_FORMAT;
        // SAFETY: the description points at the static blobs; no input
        // layout.
        let state =
            unsafe { device.CreateGraphicsPipelineState(&desc) }.at("CreateGraphicsPipelineState");
        // SAFETY: the root-signature reference is dropped once.
        unsafe { std::mem::ManuallyDrop::drop(&mut desc.pRootSignature) };
        Ok(Blit {
            root,
            state: state?,
        })
    }
}

/// Back buffers of the swap chain.
pub const BACK_BUFFERS: u32 = 2;

/// A flip-model swap chain on a window, with its back buffers.
pub struct SwapChain {
    pub swap: IDXGISwapChain3,
    pub buffers: Vec<ID3D12Resource>,
    pub width: u32,
    pub height: u32,
}

impl SwapChain {
    /// A `width` by `height` BGRA8 swap chain on `window`, presented by
    /// `gpu`'s queue.
    pub fn new(gpu: &Gpu, window: HWND, width: u32, height: u32) -> Result<SwapChain, GpuError> {
        // SAFETY: plain factory creation with no flags.
        let factory: IDXGIFactory4 =
            unsafe { CreateDXGIFactory2(DXGI_CREATE_FACTORY_FLAGS(0)) }.at("CreateDXGIFactory2")?;
        let desc = DXGI_SWAP_CHAIN_DESC1 {
            Width: width,
            Height: height,
            Format: DXGI_FORMAT_B8G8R8A8_UNORM,
            Stereo: false.into(),
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            BufferUsage: DXGI_USAGE_RENDER_TARGET_OUTPUT,
            BufferCount: BACK_BUFFERS,
            Scaling: DXGI_SCALING_STRETCH,
            SwapEffect: DXGI_SWAP_EFFECT_FLIP_DISCARD,
            ..Default::default()
        };
        // SAFETY: the queue and window are live; the description is fully
        // initialised.
        let swap: IDXGISwapChain1 =
            unsafe { factory.CreateSwapChainForHwnd(&gpu.queue, window, &desc, None, None) }
                .at("CreateSwapChainForHwnd")?;
        let swap: IDXGISwapChain3 = swap.cast().at("cast IDXGISwapChain3")?;
        let mut buffers = Vec::new();
        for i in 0..BACK_BUFFERS {
            // SAFETY: `i` is below the buffer count.
            buffers.push(unsafe { swap.GetBuffer::<ID3D12Resource>(i) }.at("GetBuffer")?);
        }
        Ok(SwapChain {
            swap,
            buffers,
            width,
            height,
        })
    }

    /// Makes the buffers again at `width` by `height` (a new back-buffer
    /// size). Every reference to the old buffers must be gone and the GPU
    /// done with them.
    pub fn resize(&mut self, width: u32, height: u32) -> Result<(), GpuError> {
        self.buffers.clear();
        println!("[swap] ResizeBuffers");
        // SAFETY: the buffer references were dropped above and the caller
        // flushed the queue; the format and flags are kept.
        unsafe {
            self.swap.ResizeBuffers(
                BACK_BUFFERS,
                width,
                height,
                DXGI_FORMAT_UNKNOWN,
                DXGI_SWAP_CHAIN_FLAG(0),
            )
        }
        .at("ResizeBuffers")?;
        for i in 0..BACK_BUFFERS {
            // SAFETY: `i` is below the buffer count.
            self.buffers
                .push(unsafe { self.swap.GetBuffer::<ID3D12Resource>(i) }.at("GetBuffer")?);
        }
        self.width = width;
        self.height = height;
        Ok(())
    }

    /// The back buffer to draw into next.
    #[must_use]
    pub fn current(&self) -> u32 {
        // SAFETY: a plain query.
        unsafe { self.swap.GetCurrentBackBufferIndex() }
    }

    /// Shows the back buffer, waiting for one vertical blank.
    pub fn present(&self) -> Result<(), GpuError> {
        // SAFETY: the back buffer was transitioned to the present state by
        // the submitted frame.
        unsafe { self.swap.Present(1, DXGI_PRESENT(0)) }
            .ok()
            .at("Present")
    }
}
