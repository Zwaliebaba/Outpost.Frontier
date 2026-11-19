//! The device calls of `IRenderPipelineEx` and `IRPDrawEx` (record 0005,
//! points 2 to 9) and the pass value.
//!
//! [`Device::begin_pass`] returns a [`Pass`] that borrows the device: while
//! it lives nothing else can draw, and a second pass cannot begin (the C++
//! refused nesting at run time). The pass starts from a copy of the
//! device's render states with `BeginExPass`'s overrides; render states
//! set in the pass change only that copy. When the pass drops, the backend
//! puts the scene target and depth buffer back; the device's states,
//! viewport, stage textures and transforms were never touched, which is
//! the C++'s restore set (target, depth surface, viewport, stage textures
//! and sampler states, every render state the pass changed, no program).

use super::super::ff::device::{Backend, Device};
use super::super::ff::draw::PipelineKey;
use super::super::ff::fvf::Fvf;
use super::super::ff::geometry::{Primitive, fan_to_list_sequential, prim_count_from_verts};
use super::super::ff::model::StateModel;
use super::super::ff::state::{Cull, RenderState, RenderStateId, ZBuffer};
use super::super::ff::textures::TextureHandle;
use super::model::{ExProgramEntry, ExTextureKind, ProgramOrigin};
use super::{
    ExBackend, ExBinding, ExCaps, ExCreate, ExDraw, ExFormat, ExPipelineKey, ExProgram, ExRelease,
    ExTexture, PassDesc, PassError, PassTarget, ProgramBlobs, QUAD_FVF, RenderTargetDesc,
    SamplerState, TEXTURE_SLOTS, fullscreen_quad,
};

impl<B: Backend + ExBackend> Device<B> {
    /// `GetExCaps`.
    #[must_use]
    pub fn ex_caps(&self) -> ExCaps {
        self.backend.ex_caps()
    }

    /// A new back-buffer size (`CreateBuffers` after a reset): the scene
    /// depth and every scaled render target take their new sizes behind
    /// the same handles.
    pub fn resize(&mut self, width: u32, height: u32) {
        self.ex.resize(width, height);
        self.backend.back_buffer_resized(width, height);
    }

    /// `CreateExProgram` for a program compiled ahead of time.
    pub fn create_program(&mut self, blobs: &ProgramBlobs) -> Result<ExProgram, PassError> {
        if blobs.vs.is_empty() || blobs.ps.is_empty() {
            return Err(PassError::InvalidParams);
        }
        let program = self.ex.insert_program(ExProgramEntry {
            name: blobs.name.to_string(),
            origin: ProgramOrigin::Compiled,
            usable: true,
        });
        let created = self.backend.ex_create(ExCreate::Program {
            program,
            vs: blobs.vs,
            ps: blobs.ps,
        });
        if let Err(e) = created {
            let _ = self.ex.remove_program(program);
            return Err(PassError::Backend(e));
        }
        Ok(program)
    }

    /// `CreateExProgram` from a source file compiled at run time
    /// (development feature `shader-compile`). As in the C++ the handle is
    /// made even when the code does not compile; it is then in the error
    /// and unusable until [`Device::reload_programs`] compiles it.
    #[cfg(feature = "shader-compile")]
    pub fn create_program_from_file(
        &mut self,
        path: &std::path::Path,
        vs_entry: &str,
        ps_entry: &str,
        compiler: &mut dyn super::ShaderCompiler,
    ) -> Result<ExProgram, PassError> {
        let program = self.ex.insert_program(ExProgramEntry {
            name: path.display().to_string(),
            origin: ProgramOrigin::File {
                path: path.to_path_buf(),
                vs_entry: vs_entry.to_string(),
                ps_entry: ps_entry.to_string(),
            },
            usable: false,
        });
        match self.compile_program(program, compiler) {
            Ok(()) => Ok(program),
            Err(log) => Err(PassError::Compile { program, log }),
        }
    }

    /// `ReloadExPrograms`: every source program is compiled again; one that
    /// fails keeps its previous code. The error holds the first failure's
    /// messages; every program is tried.
    #[cfg(feature = "shader-compile")]
    pub fn reload_programs(
        &mut self,
        compiler: &mut dyn super::ShaderCompiler,
    ) -> Result<(), PassError> {
        let mut first = None;
        for program in self.ex.programs() {
            if let Err(log) = self.compile_program(program, compiler) {
                first.get_or_insert(PassError::Compile { program, log });
            }
        }
        first.map_or(Ok(()), Err)
    }

    /// Compiles both stages of a source program and hands the backend the
    /// new code only when both compiled; a compiled program is left alone.
    #[cfg(feature = "shader-compile")]
    fn compile_program(
        &mut self,
        program: ExProgram,
        compiler: &mut dyn super::ShaderCompiler,
    ) -> Result<(), String> {
        let entry = self.ex.program(program).map_err(|e| e.to_string())?;
        let ProgramOrigin::File {
            path,
            vs_entry,
            ps_entry,
        } = entry.origin.clone()
        else {
            return Ok(());
        };
        let vs = compiler.compile(&path, &vs_entry, "vs_6_1");
        let ps = compiler.compile(&path, &ps_entry, "ps_6_1");
        let (vs, ps) = match (vs, ps) {
            (Ok(vs), Ok(ps)) => (vs, ps),
            (vs, ps) => {
                let mut log = String::new();
                for e in [vs.err(), ps.err()].into_iter().flatten() {
                    log.push_str(&e);
                }
                return Err(log);
            }
        };
        self.backend
            .ex_create(ExCreate::Program {
                program,
                vs: &vs,
                ps: &ps,
            })
            .map_err(|e| format!("{}: {e}", path.display()))?;
        self.ex
            .program_mut(program)
            .map_err(|e| e.to_string())?
            .usable = true;
        Ok(())
    }

    /// `DestroyExProgram`.
    pub fn destroy_program(&mut self, program: ExProgram) -> Result<(), PassError> {
        self.ex.remove_program(program)?;
        self.backend.ex_release(ExRelease::Program(program));
        Ok(())
    }

    /// `CreateExRenderTarget`.
    pub fn create_render_target(&mut self, desc: RenderTargetDesc) -> Result<ExTexture, PassError> {
        if !desc.size.is_valid() {
            return Err(PassError::InvalidParams);
        }
        let texture = self.ex.insert_render_target(desc);
        let t = *self.ex.texture(texture)?;
        let created = self.backend.ex_create(ExCreate::RenderTarget {
            texture,
            desc,
            width: t.width,
            height: t.height,
        });
        if let Err(e) = created {
            let _ = self.ex.remove_texture(texture);
            return Err(PassError::Backend(e));
        }
        Ok(texture)
    }

    /// `CreateExVolumeTexture`: one level of `width * height * depth`
    /// texels, tightly packed, rows then slices; `None` is all zeros. Data
    /// of another length is refused (the C++ read that many bytes
    /// whatever it was given; divergence D416).
    pub fn create_volume_texture(
        &mut self,
        width: u32,
        height: u32,
        depth: u32,
        format: ExFormat,
        texels: Option<&[u8]>,
    ) -> Result<ExTexture, PassError> {
        if width == 0 || height == 0 || depth == 0 {
            return Err(PassError::InvalidParams);
        }
        let len = u64::from(width)
            * u64::from(height)
            * u64::from(depth)
            * u64::from(format.bytes_per_texel());
        let len = usize::try_from(len).map_err(|_| PassError::InvalidParams)?;
        let zeros;
        let texels = match texels {
            Some(t) if t.len() == len => t,
            Some(_) => return Err(PassError::InvalidParams),
            None => {
                zeros = vec![0u8; len];
                &zeros
            }
        };
        let texture = self.ex.insert_volume(format, width, height, depth);
        let created = self.backend.ex_create(ExCreate::Volume {
            texture,
            format,
            width,
            height,
            depth,
            texels,
        });
        if let Err(e) = created {
            let _ = self.ex.remove_texture(texture);
            return Err(PassError::Backend(e));
        }
        Ok(texture)
    }

    /// `DestroyExTexture`: a render target or volume texture.
    pub fn destroy_ex_texture(&mut self, texture: ExTexture) -> Result<(), PassError> {
        self.ex.remove_texture(texture)?;
        self.backend.ex_release(ExRelease::Texture(texture));
        Ok(())
    }

    /// `GetExTextureSize`: width and height (a volume's depth is not
    /// reported, as in the C++).
    pub fn ex_texture_size(&self, texture: ExTexture) -> Result<(u32, u32), PassError> {
        let t = self.ex.texture(texture)?;
        Ok((t.width, t.height))
    }

    /// `GetSceneDepth`.
    #[must_use]
    pub fn scene_depth(&self) -> ExTexture {
        self.ex.scene_depth()
    }

    /// `BeginExPass`: binds the target (and the depth buffer with
    /// `keep_depth`), sets the pass's starting states and clears when
    /// asked. The pass ends when the value drops.
    pub fn begin_pass(&mut self, desc: PassDesc) -> Result<Pass<'_, B>, PassError> {
        let mut state = self.state.clone();
        // The pass's starting render states (`BeginExPass`).
        for s in [
            RenderState::AlphaBlendEnable(false),
            RenderState::AlphaTestEnable(false),
            RenderState::FogEnable(false),
            RenderState::StencilEnable(false),
            RenderState::CullMode(Cull::None),
            RenderState::Lighting(false),
            RenderState::ColorWriteEnable(0xf),
        ] {
            state.set_render_state(s);
        }
        let mut pass = Pass {
            device: self,
            state,
            target: PassTarget::Scene,
            target_size: (0, 0),
            target_format: ExFormat::Bgra8,
            depth_bound: true,
            program: None,
            textures: [None; TEXTURE_SLOTS],
        };
        // A failed selection ends the pass at once (the drop), as
        // `BeginExPass` runs `ExEndPassInternal`.
        pass.set_target(desc.target, desc.keep_depth)?;
        if let Some(color) = desc.clear {
            pass.device.backend.ex_clear(pass.target, color);
        }
        Ok(pass)
    }
}

/// An open pass (`BeginExPass` to `EndExPass`). Dropping it ends the pass.
pub struct Pass<'d, B: Backend + ExBackend> {
    device: &'d mut Device<B>,
    state: StateModel,
    target: PassTarget,
    target_size: (u32, u32),
    target_format: ExFormat,
    depth_bound: bool,
    program: Option<ExProgram>,
    textures: [Option<(ExBinding, SamplerState)>; TEXTURE_SLOTS],
}

impl<B: Backend + ExBackend> Drop for Pass<'_, B> {
    fn drop(&mut self) {
        self.device.backend.ex_end_pass();
    }
}

impl<B: Backend + ExBackend> Pass<'_, B> {
    /// `EndExPass`.
    pub fn end(self) {}

    /// The device, for reading: its textures, buffers and states as they
    /// were when the pass began.
    #[must_use]
    pub fn device(&self) -> &Device<B> {
        self.device
    }

    /// The backend, for reading results in tests.
    pub fn backend_mut(&mut self) -> &mut B {
        &mut self.device.backend
    }

    /// `SetRenderState` inside the pass: the pass's copy only.
    pub fn set_render_state(&mut self, state: RenderState) {
        self.state.set_render_state(state);
    }

    #[must_use]
    pub fn render_state(&self, id: RenderStateId) -> RenderState {
        self.state.render_state(id)
    }

    #[must_use]
    pub fn target(&self) -> PassTarget {
        self.target
    }

    /// The current target's width and height.
    #[must_use]
    pub fn target_size(&self) -> (u32, u32) {
        self.target_size
    }

    /// Whether the scene depth is bound as the depth buffer.
    #[must_use]
    pub fn depth_bound(&self) -> bool {
        self.depth_bound
    }

    /// No depth buffer: depth test and write off too (`ExUnbindDepth`).
    fn unbind_depth(&mut self) {
        self.depth_bound = false;
        self.state
            .set_render_state(RenderState::ZEnable(ZBuffer::Off));
        self.state
            .set_render_state(RenderState::ZWriteEnable(false));
    }

    /// `SetExRenderTarget` (and the target selection of `BeginExPass`): a
    /// texture the pass sampled is unbound from its slots, the viewport
    /// becomes the whole target, and the depth buffer is bound with
    /// `keep_depth` (unbinding the scene depth from its slots) or not at
    /// all. Binding the depth leaves the depth states as they are.
    pub fn set_target(&mut self, target: PassTarget, keep_depth: bool) -> Result<(), PassError> {
        let (size, format) = match target {
            PassTarget::Scene => (self.device.ex.back_buffer(), ExFormat::Bgra8),
            PassTarget::Texture(t) => {
                let entry = self
                    .device
                    .ex
                    .texture(t)
                    .map_err(|_| PassError::InvalidParams)?;
                if !matches!(entry.kind, ExTextureKind::RenderTarget(_)) {
                    return Err(PassError::NotRenderTarget);
                }
                ((entry.width, entry.height), entry.format)
            }
        };
        if let PassTarget::Texture(t) = target {
            for slot in &mut self.textures {
                if matches!(slot, Some((ExBinding::Ex(b), _)) if *b == t) {
                    *slot = None;
                }
            }
        }
        self.target = target;
        self.target_size = size;
        self.target_format = format;
        if keep_depth {
            let depth = self.device.ex.scene_depth();
            for slot in &mut self.textures {
                if matches!(slot, Some((ExBinding::Ex(b), _)) if *b == depth) {
                    *slot = None;
                }
            }
            self.depth_bound = true;
        } else {
            self.unbind_depth();
        }
        Ok(())
    }

    /// `BindExProgram`; `None` unbinds (the C++'s return to fixed function,
    /// which a pass here cannot draw with).
    pub fn bind_program(&mut self, program: Option<ExProgram>) -> Result<(), PassError> {
        if let Some(p) = program
            && !self.device.ex.program(p)?.usable
        {
            return Err(PassError::ProgramUnusable);
        }
        self.program = program;
        Ok(())
    }

    /// `SetExVertexConstants`: registers from `start` on; they stay set
    /// after the pass.
    pub fn set_vertex_constants(&mut self, start: u32, data: &[[f32; 4]]) -> Result<(), PassError> {
        self.device.ex.set_vertex_constants(start, data)
    }

    /// `SetExPixelConstants`.
    pub fn set_pixel_constants(&mut self, start: u32, data: &[[f32; 4]]) -> Result<(), PassError> {
        self.device.ex.set_pixel_constants(start, data)
    }

    /// `BindExTexture`: `None` unbinds. The current target cannot be
    /// sampled; binding the scene depth while it is the depth buffer
    /// unbinds the depth buffer for the rest of the pass, or until a target
    /// change binds it again.
    pub fn bind_texture(
        &mut self,
        slot: u32,
        texture: Option<ExTexture>,
        sampler: SamplerState,
    ) -> Result<(), PassError> {
        let index = slot as usize;
        if index >= TEXTURE_SLOTS {
            return Err(PassError::InvalidParams);
        }
        let Some(t) = texture else {
            self.textures[index] = None;
            return Ok(());
        };
        self.device.ex.texture(t)?;
        if self.target == PassTarget::Texture(t) {
            return Err(PassError::TargetBound);
        }
        if t == self.device.ex.scene_depth() && self.depth_bound {
            self.unbind_depth();
        }
        self.textures[index] = Some((ExBinding::Ex(t), sampler));
        Ok(())
    }

    /// `BindExPipelineTexture`: a texture of the fixed-function device.
    pub fn bind_device_texture(
        &mut self,
        slot: u32,
        texture: Option<TextureHandle>,
        sampler: SamplerState,
    ) -> Result<(), PassError> {
        let index = slot as usize;
        if index >= TEXTURE_SLOTS {
            return Err(PassError::InvalidParams);
        }
        if let Some(t) = texture {
            self.device.textures.get(t).map_err(|_| PassError::Stale)?;
        }
        self.textures[index] = texture.map(|t| (ExBinding::Device(t), sampler));
        Ok(())
    }

    /// The slot bindings.
    #[must_use]
    pub fn textures(&self) -> &[Option<(ExBinding, SamplerState)>; TEXTURE_SLOTS] {
        &self.textures
    }

    /// `DrawFullscreenQuad`: two triangles over the whole target.
    pub fn draw_fullscreen_quad(&mut self) -> Result<(), PassError> {
        let quad = fullscreen_quad();
        self.submit(Primitive::TriangleStrip, QUAD_FVF, quad, 4, None)
    }

    /// `DrawExPrimitiveUp`: `count` vertices of `fvf` with the program
    /// bound; `prim` is a `D3DPRIMITIVETYPE`.
    pub fn draw(
        &mut self,
        prim: u32,
        fvf: Fvf,
        vertices: &[u8],
        count: u32,
    ) -> Result<(), PassError> {
        let primitive = Primitive::from_raw(prim).ok_or(PassError::InvalidParams)?;
        let primitive_count = prim_count_from_verts(prim, count);
        if primitive_count.cast_signed() <= 0 {
            return Err(PassError::InvalidParams);
        }
        let bytes = (fvf.stride() as usize)
            .checked_mul(count as usize)
            .ok_or(PassError::InvalidParams)?;
        let data = vertices
            .get(..bytes)
            .ok_or(PassError::InvalidParams)?
            .to_vec();
        if primitive == Primitive::TriangleFan {
            let list = fan_to_list_sequential(0, primitive_count as usize);
            return self.submit(Primitive::TriangleList, fvf, data, count, Some(list));
        }
        self.submit(primitive, fvf, data, count, None)
    }

    fn submit(
        &mut self,
        primitive: Primitive,
        fvf: Fvf,
        vertices: Vec<u8>,
        vertex_count: u32,
        indices: Option<Vec<u16>>,
    ) -> Result<(), PassError> {
        let program = self.program.ok_or(PassError::NoProgram)?;
        if !self.device.ex.program(program)?.usable {
            return Err(PassError::ProgramUnusable);
        }
        let mut base = PipelineKey::new(&self.state, fvf, primitive);
        base.flat = false;
        if !self.depth_bound {
            base.depth = None;
            base.depth_bias = 0;
        }
        let draw = ExDraw {
            key: ExPipelineKey {
                program,
                base,
                target_format: self.target_format,
                depth_bound: self.depth_bound,
            },
            target: self.target,
            target_size: self.target_size,
            vertex_constants: self.device.ex.vertex_constants().to_vec(),
            pixel_constants: self.device.ex.pixel_constants().to_vec(),
            textures: self.textures.map(|slot| {
                slot.map(|(binding, sampler)| match binding {
                    ExBinding::Ex(t) if t == self.device.ex.scene_depth() => {
                        (ExBinding::SceneDepth, sampler)
                    }
                    other => (other, sampler),
                })
            }),
            primitive,
            fvf,
            stride: fvf.stride(),
            vertices,
            vertex_count,
            indices,
        };
        // Device textures the draw samples reach the backend first.
        self.device.sync_resources();
        self.device.backend.ex_draw(draw);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::super::recording::ExCreateRecord;
    use super::super::{Address, Filter, MipFilter, TargetSize};
    use super::*;
    use crate::render::ff::defaults::Defaults;
    use crate::render::ff::device::RecordingBackend;
    use crate::render::ff::state::{Blend, Compare};
    use neuron_core::image::PixelFormat;

    const BLOBS: ProgramBlobs = ProgramBlobs {
        name: "test",
        vs: &[1, 2, 3],
        ps: &[4, 5],
    };

    fn device() -> Device<RecordingBackend> {
        Device::new(Defaults::builtin(), 640, 480, RecordingBackend::default())
    }

    fn target(d: &mut Device<RecordingBackend>, scale: f32) -> ExTexture {
        d.create_render_target(RenderTargetDesc {
            size: TargetSize::Scaled(scale),
            format: ExFormat::Bgra8,
        })
        .unwrap()
    }

    fn scene_pass(d: &mut Device<RecordingBackend>) -> Pass<'_, RecordingBackend> {
        d.begin_pass(PassDesc {
            target: PassTarget::Scene,
            keep_depth: false,
            clear: None,
        })
        .unwrap()
    }

    #[test]
    fn a_pass_leaves_the_device_as_it_found_it() {
        let mut d = device();
        d.state
            .set_render_state(RenderState::CullMode(Cull::Clockwise));
        d.state.set_render_state(RenderState::ZFunc(Compare::Less));
        d.set_viewport(10, 20, 30, 40);
        let before = d.state.clone();
        let program = d.create_program(&BLOBS).unwrap();
        let rt = target(&mut d, 0.5);
        {
            let mut pass = d
                .begin_pass(PassDesc {
                    target: PassTarget::Texture(rt),
                    keep_depth: false,
                    clear: Some(0xff00_00ff),
                })
                .unwrap();
            // The starting states: no blend, no cull, all channels, no depth.
            assert_eq!(
                pass.render_state(RenderStateId::CullMode),
                RenderState::CullMode(Cull::None)
            );
            assert_eq!(
                pass.render_state(RenderStateId::ZEnable),
                RenderState::ZEnable(ZBuffer::Off)
            );
            assert_eq!(pass.target_size(), (320, 240));
            assert_eq!(pass.draw_fullscreen_quad(), Err(PassError::NoProgram));
            pass.bind_program(Some(program)).unwrap();
            pass.set_render_state(RenderState::AlphaBlendEnable(true));
            pass.set_render_state(RenderState::SrcBlend(Blend::One));
            pass.set_render_state(RenderState::DestBlend(Blend::SrcAlpha));
            pass.draw_fullscreen_quad().unwrap();
        }
        // Nothing the pass set reached the device.
        for id in [
            RenderStateId::AlphaBlendEnable,
            RenderStateId::SrcBlend,
            RenderStateId::CullMode,
            RenderStateId::ZEnable,
            RenderStateId::ZFunc,
        ] {
            assert_eq!(d.state.render_state(id), before.render_state(id), "{id:?}");
        }
        assert_eq!(d.viewport().width, 30);
        let ex = &d.backend.ex;
        assert_eq!(ex.passes_ended, 1);
        assert_eq!(ex.clears, [(PassTarget::Texture(rt), 0xff00_00ff)]);
        let draw = &ex.draws[0];
        assert_eq!(draw.target_size, (320, 240));
        assert_eq!(draw.primitive, Primitive::TriangleStrip);
        assert_eq!(draw.vertices, fullscreen_quad());
        assert_eq!(
            draw.key.base.blend.map(|b| (b.0, b.1)),
            Some((Blend::One, Blend::SrcAlpha))
        );
        assert_eq!(draw.key.base.depth, None);
        assert_eq!(draw.key.base.cull, Cull::None);
        assert!(!draw.key.depth_bound);
        assert_eq!(draw.key.program, program);
    }

    #[test]
    fn keep_depth_inherits_the_device_depth_states() {
        let mut d = device();
        d.state.set_render_state(RenderState::ZEnable(ZBuffer::On));
        d.state.set_render_state(RenderState::ZWriteEnable(false));
        d.state
            .set_render_state(RenderState::ZFunc(Compare::Greater));
        let program = d.create_program(&BLOBS).unwrap();
        let depth = d.scene_depth();
        let mut pass = d
            .begin_pass(PassDesc {
                target: PassTarget::Scene,
                keep_depth: true,
                clear: None,
            })
            .unwrap();
        assert_eq!(pass.target_size(), (640, 480));
        pass.bind_program(Some(program)).unwrap();
        pass.draw_fullscreen_quad().unwrap();
        // Sampling the scene depth unbinds the depth buffer.
        pass.bind_texture(0, Some(depth), SamplerState::default())
            .unwrap();
        assert!(!pass.depth_bound());
        pass.draw_fullscreen_quad().unwrap();
        // Binding the depth again unbinds the scene depth from its slot and
        // leaves the depth states off.
        pass.set_target(PassTarget::Scene, true).unwrap();
        assert_eq!(pass.textures()[0], None);
        pass.draw_fullscreen_quad().unwrap();
        drop(pass);
        let draws = &d.backend.ex.draws;
        assert_eq!(draws[0].key.base.depth, Some((Compare::Greater, false)));
        assert!(draws[0].key.depth_bound);
        assert_eq!(draws[1].key.base.depth, None);
        assert_eq!(
            draws[1].textures[0],
            Some((ExBinding::SceneDepth, SamplerState::default()))
        );
        assert!(draws[2].key.depth_bound);
        assert_eq!(draws[2].key.base.depth, None);
    }

    #[test]
    fn targets_and_slots_follow_the_cpp_rules() {
        let mut d = device();
        let a = target(&mut d, 0.25);
        let b = target(&mut d, 1.0);
        let volume = d
            .create_volume_texture(2, 2, 2, ExFormat::L8, None)
            .unwrap();
        let gone = d
            .textures
            .create(4, 4, PixelFormat::A8R8G8B8, 1, 0)
            .unwrap();
        d.destroy_texture(gone).unwrap();
        // A volume is no target; the failed pass ends at once.
        let refused = d
            .begin_pass(PassDesc {
                target: PassTarget::Texture(volume),
                keep_depth: false,
                clear: None,
            })
            .err();
        assert_eq!(refused, Some(PassError::NotRenderTarget));
        assert_eq!(d.backend.ex.passes_ended, 1);
        let linear = SamplerState::new(Filter::Linear, MipFilter::None, Address::Wrap);
        let mut pass = d
            .begin_pass(PassDesc {
                target: PassTarget::Texture(a),
                keep_depth: false,
                clear: None,
            })
            .unwrap();
        assert_eq!(
            pass.bind_texture(0, Some(a), linear),
            Err(PassError::TargetBound)
        );
        assert_eq!(
            pass.bind_texture(16, Some(b), linear),
            Err(PassError::InvalidParams)
        );
        pass.bind_texture(3, Some(b), linear).unwrap();
        pass.bind_texture(4, Some(volume), linear).unwrap();
        // Drawing into b unbinds it from slot 3.
        pass.set_target(PassTarget::Texture(b), false).unwrap();
        assert_eq!(pass.textures()[3], None);
        assert_eq!(pass.textures()[4], Some((ExBinding::Ex(volume), linear)));
        assert_eq!(pass.target_size(), (640, 480));
        assert_eq!(
            pass.set_target(PassTarget::Texture(volume), false),
            Err(PassError::NotRenderTarget)
        );
        assert_eq!(
            pass.bind_device_texture(1, Some(gone), linear),
            Err(PassError::Stale)
        );
        drop(pass);
        assert_eq!(d.backend.ex.passes_ended, 2);
        // A destroyed texture's handle is stale; the scene depth stays.
        d.destroy_ex_texture(b).unwrap();
        assert_eq!(d.ex_texture_size(b), Err(PassError::Stale));
        assert_eq!(
            d.destroy_ex_texture(d.scene_depth()),
            Err(PassError::SceneDepth)
        );
        assert_eq!(d.backend.ex.released, [ExRelease::Texture(b)]);
    }

    #[test]
    fn device_textures_bind_and_fans_arrive_as_lists() {
        let mut d = device();
        let program = d.create_program(&BLOBS).unwrap();
        let t = d
            .textures
            .create(4, 4, PixelFormat::A8R8G8B8, 1, 0)
            .unwrap();
        let mut pass = scene_pass(&mut d);
        pass.bind_program(Some(program)).unwrap();
        pass.bind_device_texture(2, Some(t), SamplerState::default())
            .unwrap();
        // XYZ | DIFFUSE: 16 bytes a vertex; a fan of five vertices.
        let verts = vec![0u8; 16 * 5];
        pass.draw(6, Fvf(0x42), &verts, 5).unwrap();
        assert_eq!(
            pass.draw(4, Fvf(0x42), &verts, 2),
            Err(PassError::InvalidParams)
        );
        assert_eq!(
            pass.draw(4, Fvf(0x42), &verts, 6),
            Err(PassError::InvalidParams)
        );
        drop(pass);
        let draw = &d.backend.ex.draws[0];
        assert_eq!(draw.primitive, Primitive::TriangleList);
        assert_eq!(draw.indices, Some(vec![0, 1, 2, 0, 2, 3, 0, 3, 4]));
        assert_eq!(draw.stride, 16);
        assert_eq!(
            draw.textures[2],
            Some((ExBinding::Device(t), SamplerState::default()))
        );
        // The texture reached the backend before the draw.
        assert!(!d.backend.resources.is_empty());
    }

    #[test]
    fn constants_outlive_the_pass() {
        let mut d = device();
        let program = d.create_program(&BLOBS).unwrap();
        for first in [true, false] {
            let mut pass = scene_pass(&mut d);
            pass.bind_program(Some(program)).unwrap();
            if first {
                pass.set_pixel_constants(5, &[[1.0; 4]]).unwrap();
                pass.set_vertex_constants(0, &[[2.0; 4]]).unwrap();
                assert_eq!(
                    pass.set_pixel_constants(224, &[[0.0; 4]]),
                    Err(PassError::InvalidParams)
                );
            }
            pass.draw_fullscreen_quad().unwrap();
        }
        let draws = &d.backend.ex.draws;
        assert_eq!(draws[1].pixel_constants[5], [1.0; 4]);
        assert_eq!(draws[1].vertex_constants[0], [2.0; 4]);
        assert_eq!(draws[1].pixel_constants.len(), 224);
        assert_eq!(draws[1].vertex_constants.len(), 256);
    }

    #[test]
    fn creation_validates_and_falls_back_like_the_nebula() {
        let mut d = device();
        d.backend.ex.refuse_targets.push(ExFormat::Rgba16Float);
        let half = d.create_render_target(RenderTargetDesc {
            size: TargetSize::Scaled(0.5),
            format: ExFormat::Rgba16Float,
        });
        assert!(matches!(half, Err(PassError::Backend(_))));
        let half = d
            .create_render_target(RenderTargetDesc {
                size: TargetSize::Scaled(0.5),
                format: ExFormat::Bgra8,
            })
            .unwrap();
        assert_eq!(
            d.create_render_target(RenderTargetDesc {
                size: TargetSize::Scaled(-1.0),
                format: ExFormat::Bgra8,
            }),
            Err(PassError::InvalidParams)
        );
        assert_eq!(
            d.create_volume_texture(2, 2, 2, ExFormat::Bgra8, Some(&[0; 31])),
            Err(PassError::InvalidParams)
        );
        assert_eq!(
            d.create_volume_texture(0, 2, 2, ExFormat::Bgra8, None),
            Err(PassError::InvalidParams)
        );
        let v = d
            .create_volume_texture(2, 2, 2, ExFormat::Rgba16Float, None)
            .unwrap();
        assert_eq!(
            d.create_program(&ProgramBlobs {
                name: "empty",
                vs: &[],
                ps: &[1],
            }),
            Err(PassError::InvalidParams)
        );
        d.resize(100, 60);
        assert_eq!(d.ex_texture_size(half), Ok((50, 30)));
        assert_eq!(d.ex_texture_size(d.scene_depth()), Ok((100, 60)));
        assert_eq!(d.ex_texture_size(v), Ok((2, 2)));
        let ex = &d.backend.ex;
        assert_eq!(ex.resizes, [(100, 60)]);
        assert_eq!(
            ex.created,
            [
                ExCreateRecord::RenderTarget {
                    texture: half,
                    format: ExFormat::Bgra8,
                    width: 320,
                    height: 240
                },
                ExCreateRecord::Volume {
                    texture: v,
                    format: ExFormat::Rgba16Float,
                    size: [2, 2, 2],
                    texels: 64
                },
            ]
        );
    }

    #[cfg(feature = "shader-compile")]
    #[test]
    fn reloads_keep_the_code_that_compiled() {
        use std::path::Path;
        /// Compiles while `ok`; the blob names the entry.
        struct Fake {
            ok: bool,
        }
        impl super::super::ShaderCompiler for Fake {
            fn compile(&mut self, _: &Path, entry: &str, _: &str) -> Result<Vec<u8>, String> {
                if self.ok {
                    Ok(entry.as_bytes().to_vec())
                } else {
                    Err(format!("{entry}: error X3000"))
                }
            }
        }
        let mut d = device();
        let mut fake = Fake { ok: false };
        let path = Path::new("shaders/nebula.hlsl");
        let Err(PassError::Compile { program, log }) =
            d.create_program_from_file(path, "vs_main", "ps_main", &mut fake)
        else {
            panic!("the program should not compile")
        };
        assert!(log.contains("X3000"));
        {
            let mut pass = scene_pass(&mut d);
            assert_eq!(
                pass.bind_program(Some(program)),
                Err(PassError::ProgramUnusable)
            );
        }
        fake.ok = true;
        d.reload_programs(&mut fake).unwrap();
        assert!(d.ex.program(program).unwrap().usable);
        fake.ok = false;
        assert!(d.reload_programs(&mut fake).is_err());
        // The previous code stays.
        assert!(d.ex.program(program).unwrap().usable);
        assert_eq!(d.backend.ex.created.len(), 1);
    }
}
