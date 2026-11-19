//! The nodes of the effect graph (`FGenericFxObjectBase` and its 19
//! classes in `particlefx_genericfx.h`, `particlefx_emitters.h`,
//! `particlefx_fields.h`, `particlefx_appearances.h`): the shared node
//! state, the tree links, and per family the state its update keeps.
//!
//! Nodes live in [`super::ParticleFx`]'s arena and are named by
//! [`NodeId`]; a node keeps the reference count the reference keeps, and is
//! destroyed (its particles returned to the pools, its texture and its
//! references released) when the count reaches zero (D374).

use std::rc::Rc;

use neuron_core::fx_data::NodeClass;

use super::props::NodeProps;
use super::ramp::IDENTITY;
use super::services::FrameOut;

/// A node in the arena: a slot index and the slot's generation.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct NodeId {
    pub(crate) index: u32,
    pub(crate) generation: u32,
}

/// Flag bit 0: the node has finished (`TickLifetime`, an appearance with
/// no emitter and no particle). The update walk submits it.
pub const FLAG_DONE: u32 = 0x1;
/// Flag bit 1: the node's animated transform is not evaluated.
pub const FLAG_NO_ANIMATION: u32 = 0x2;
/// Flag bit 2: the raw (local) transform was set from outside.
pub const FLAG_LOCAL_SET: u32 = 0x4;
/// Flag bit 3: the composed (world) transform was set from outside.
pub const FLAG_WORLD_SET: u32 = 0x8;
/// Flag bit 31: the node was submitted (`IsNodeDead`).
pub const FLAG_DEAD: u32 = 0x8000_0000;

/// The node families, which order siblings (`GetSortKey`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Family {
    /// `FxNode`.
    Folder,
    /// The three emitters.
    Emitter,
    /// The seven fields.
    Field,
    /// The eight appearances.
    Appearance,
}

impl Family {
    /// The family of a class.
    #[must_use]
    pub const fn of(class: NodeClass) -> Self {
        match class {
            NodeClass::Node => Self::Folder,
            NodeClass::SphereEmitter | NodeClass::ConeEmitter | NodeClass::CubeEmitter => {
                Self::Emitter
            }
            NodeClass::AirField
            | NodeClass::GravityField
            | NodeClass::CollideField
            | NodeClass::RadialField
            | NodeClass::TurbulenceField
            | NodeClass::DustField
            | NodeClass::BeamField => Self::Field,
            _ => Self::Appearance,
        }
    }

    /// `GetSortKey`: appearances 0x64, fields 0xc8, emitters 0x12c, folders
    /// 0x190. `LinkNodeTree` keeps siblings in ascending order.
    #[must_use]
    pub const fn sort_key(self) -> i32 {
        match self {
            Self::Appearance => 0x64,
            Self::Field => 0xC8,
            Self::Emitter => 0x12C,
            Self::Folder => 0x190,
        }
    }
}

/// The shape parameters an emitter's update evaluates before emitting.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ShapeState {
    /// Cone and sphere: the minimum radius.
    pub min_radius: f32,
    /// Cone and sphere: maximum less minimum radius.
    pub radius_range: f32,
    /// Cone and cube: the minimum spread in radians.
    pub min_spread: f32,
    /// Cone and cube: maximum less minimum spread in radians.
    pub spread_range: f32,
    /// Cube: the extents.
    pub width: f32,
    /// Cube.
    pub height: f32,
    /// Cube.
    pub depth: f32,
}

/// An emitter's state (`FFxEmitterBase` and the shape classes).
#[derive(Clone, Debug, PartialEq)]
pub struct EmitterState {
    /// The appearance it feeds (holding a reference).
    pub appearance: Option<NodeId>,
    /// The age at the last emission.
    pub last_emit: f32,
    /// `Emitter_Pressure` at this update.
    pub pressure: f32,
    /// `Emitter_InitLifeSpan` at this update: a particle's duration.
    pub init_life_span: f32,
    /// The next update adds the initial particles and takes the position.
    pub reset_history: bool,
    /// The world position at the last emission.
    pub prev_pos: [f32; 3],
    /// The fraction of a particle carried to the next update.
    pub fraction: f32,
    /// The emitter in its appearance's space.
    pub xform: [f32; 12],
    /// The shape parameters.
    pub shape: ShapeState,
}

impl Default for EmitterState {
    fn default() -> Self {
        Self {
            appearance: None,
            last_emit: 0.0,
            pressure: 0.0,
            init_life_span: 0.0,
            reset_history: true,
            prev_pos: [0.0; 3],
            fraction: 0.0,
            xform: IDENTITY,
            shape: ShapeState::default(),
        }
    }
}

/// A field's state: what its update evaluates and what `BeginField` derives
/// for the appearance it acts on. The members a class does not use stay at
/// their construction values.
#[derive(Clone, Debug, PartialEq)]
pub struct FieldState {
    /// Gravity's step, air's and radial's and turbulence's blend inputs.
    pub magnitude: f32,
    /// Air, radial, turbulence: the approach.
    pub approach: f32,
    /// Air, radial, turbulence: `min(dt * approach * 4, 1)`.
    pub blend: f32,
    /// Radial: one over the radius.
    pub dist_scale: f32,
    /// Gravity, air: the pull; radial: the centre (appearance space).
    pub cache: [f32; 3],
    /// Collide: the reflectivity.
    pub reflect: f32,
    /// Collide, dust: the last update's step.
    pub update_dt: f32,
    /// Dust: the radius and its square.
    pub radius: f32,
    /// Dust.
    pub radius_sq: f32,
    /// Collide, dust: the field in the appearance's space.
    pub begin_xform: [f32; 12],
    /// Collide: the plane normal, a point on the plane, the normal times 5.
    pub normal: [f32; 3],
    /// Collide.
    pub corner: [f32; 3],
    /// Collide.
    pub scaled_normal: [f32; 3],
}

impl Default for FieldState {
    fn default() -> Self {
        Self {
            magnitude: 0.0,
            approach: 0.0,
            blend: 0.0,
            dist_scale: 0.0,
            cache: [0.0; 3],
            reflect: 0.0,
            update_dt: 0.0,
            radius: 0.0,
            radius_sq: 0.0,
            begin_xform: IDENTITY,
            normal: [0.0; 3],
            corner: [0.0; 3],
            scaled_normal: [0.0; 3],
        }
    }
}

/// The texture state of a billboard appearance (`FFxTextureWrapper`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TextureWrapper {
    /// The texture name's CRC, 0 for none.
    pub crc: u32,
    /// The playback id, -1 for none.
    pub handle: i32,
    /// The current frame (`GetFrame`'s five words).
    pub frame: FrameOut,
    /// The record's frame count.
    pub frame_count: i32,
}

impl Default for TextureWrapper {
    fn default() -> Self {
        Self {
            crc: 0,
            handle: -1,
            frame: FrameOut {
                handle: 0,
                uv: [0; 4],
            },
            frame_count: 0,
        }
    }
}

/// An appearance's state: its fields, the emitters feeding it, its own
/// particle list (threaded through the pool's items) and its texture.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AppearanceState {
    /// The fields acting on its particles, holding references; `None` for
    /// a pair whose second node is no field.
    pub fields: Vec<Option<NodeId>>,
    /// The number of emitters associated with it (`AddEmitterRef`).
    pub emitter_refs: i32,
    /// Its live particles (`LiveInstanceCount_`).
    pub live: u32,
    /// The first and last particle of its list (pool item indices).
    pub head: Option<u32>,
    /// The last particle.
    pub tail: Option<u32>,
    /// The texture.
    pub texture: TextureWrapper,
    /// Dust: the last update's step.
    pub last_dt: f32,
    /// Dust: the camera transform the draw last used.
    pub view_xform: [f32; 12],
    /// Beam: flips with every particle.
    pub ribbon_parity: bool,
    /// Dust: the camera position of the last draw (`LastCameraPos_`).
    pub last_camera_pos: [f32; 3],
    /// Dust: the camera step length of the last draw (`LastCameraStep_`).
    pub last_camera_step: f32,
    /// The next appearance of its batch list (`BatchNext_`).
    pub batch_next: Option<NodeId>,
    /// The previous appearance of its batch list (`BatchPrev_`).
    pub batch_prev: Option<NodeId>,
}

/// What a node does on its update.
#[derive(Clone, Debug, PartialEq)]
pub enum Behaviour {
    /// `FxNode` and `FLBeamField`'s update: only the lifetime.
    Folder,
    /// An emitter.
    Emitter(Box<EmitterState>),
    /// A field.
    Field(Box<FieldState>),
    /// An appearance.
    Appearance(Box<AppearanceState>),
}

/// One node.
#[derive(Clone, Debug)]
pub struct Node {
    pub(crate) refs: u32,
    /// The flags (`FLAG_*`).
    pub flags: u32,
    /// The shared property set.
    pub props: Rc<NodeProps>,
    /// The parent in the tree.
    pub parent: Option<NodeId>,
    /// The first child.
    pub first_child: Option<NodeId>,
    /// The previous sibling.
    pub prev: Option<NodeId>,
    /// The next sibling.
    pub next: Option<NodeId>,
    /// The raw transform (slot 1).
    pub local: [f32; 12],
    /// The composed transform (slot 0).
    pub world: [f32; 12],
    /// The age.
    pub age: f32,
    /// The duration: past it the node is done.
    pub duration: f32,
    /// The outer time the ramps are evaluated at.
    pub outer_time: f32,
    /// The LOD input of the LOD curves.
    pub lod_input: f32,
    /// The family's state.
    pub behaviour: Behaviour,
}

impl Node {
    /// A node as the factory makes it: flags 0, identity transforms, LOD
    /// input 1, the duration the set's life span.
    #[must_use]
    pub fn new(props: Rc<NodeProps>) -> Self {
        let behaviour = match Family::of(props.class) {
            _ if props.class == NodeClass::BeamField => Behaviour::Folder,
            Family::Folder => Behaviour::Folder,
            Family::Emitter => Behaviour::Emitter(Box::default()),
            Family::Field => Behaviour::Field(Box::default()),
            Family::Appearance => Behaviour::Appearance(Box::new(AppearanceState {
                view_xform: IDENTITY,
                // `FLDustAppearance`'s constructor: 0x3d4ccccd.
                last_dt: f32::from_bits(0x3D4C_CCCD),
                ..AppearanceState::default()
            })),
        };
        Self {
            refs: 1,
            flags: 0,
            duration: props.life_span,
            props,
            parent: None,
            first_child: None,
            prev: None,
            next: None,
            local: IDENTITY,
            world: IDENTITY,
            age: 0.0,
            outer_time: 0.0,
            lod_input: 1.0,
            behaviour,
        }
    }

    /// The node's class.
    #[must_use]
    pub fn class(&self) -> NodeClass {
        self.props.class
    }

    /// The node's family.
    #[must_use]
    pub fn family(&self) -> Family {
        Family::of(self.props.class)
    }

    /// `IsNodeDead`: flag bit 31.
    #[must_use]
    pub const fn is_dead(&self) -> bool {
        self.flags & FLAG_DEAD != 0
    }

    /// The reference count.
    #[must_use]
    pub const fn refs(&self) -> u32 {
        self.refs
    }

    /// The emitter state.
    #[must_use]
    pub fn emitter(&self) -> Option<&EmitterState> {
        match &self.behaviour {
            Behaviour::Emitter(e) => Some(e),
            _ => None,
        }
    }

    /// The field state.
    #[must_use]
    pub fn field(&self) -> Option<&FieldState> {
        match &self.behaviour {
            Behaviour::Field(f) => Some(f),
            _ => None,
        }
    }

    /// The appearance state.
    #[must_use]
    pub fn appearance(&self) -> Option<&AppearanceState> {
        match &self.behaviour {
            Behaviour::Appearance(a) => Some(a),
            _ => None,
        }
    }

    pub(crate) fn appearance_mut(&mut self) -> Option<&mut AppearanceState> {
        match &mut self.behaviour {
            Behaviour::Appearance(a) => Some(a),
            _ => None,
        }
    }
}
