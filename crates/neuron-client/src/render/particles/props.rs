//! Node property sets at run time: `neuron_core::fx_data::PropertySet`
//! with its ramps made evaluable. A prototype in the node library and every
//! clone of it share one set (the reference's clones copy the set pointer),
//! so a set is handed around as an `Rc`.

use neuron_core::fx_data::{self as fx, ClassData, EmitterShape, Field, NodeClass};

use super::ramp::{AnimatedRamp, AnimatedTransform, ColorRampRamp, NestedSingle, SingleRampRamp};

/// The emitter shape ramps.
#[derive(Clone, Debug, PartialEq)]
pub enum ShapeProps {
    /// `FxConeEmitter`.
    Cone {
        /// `ConeEmitter_MinRadius`.
        min_radius: AnimatedRamp,
        /// `ConeEmitter_MaxRadius`.
        max_radius: AnimatedRamp,
        /// `ConeEmitter_MinSpread`, degrees.
        min_spread: AnimatedRamp,
        /// `ConeEmitter_MaxSpread`, degrees.
        max_spread: AnimatedRamp,
    },
    /// `FxCubeEmitter`.
    Cube {
        /// `CubeEmitter_Width`.
        width: AnimatedRamp,
        /// `CubeEmitter_Height`.
        height: AnimatedRamp,
        /// `CubeEmitter_Depth`.
        depth: AnimatedRamp,
        /// `CubeEmitter_MinSpread`, degrees.
        min_spread: AnimatedRamp,
        /// `CubeEmitter_MaxSpread`, degrees.
        max_spread: AnimatedRamp,
    },
    /// `FxSphereEmitter`.
    Sphere {
        /// `SphereEmitter_MinRadius`.
        min_radius: AnimatedRamp,
        /// `SphereEmitter_MaxRadius`.
        max_radius: AnimatedRamp,
    },
}

/// The emitter set.
#[derive(Clone, Debug, PartialEq)]
pub struct EmitterProps {
    /// `Emitter_Frequency`.
    pub frequency: AnimatedRamp,
    /// `Emitter_MaxParticles`.
    pub max_particles: AnimatedRamp,
    /// `Emitter_InitLifeSpan`.
    pub init_life_span: AnimatedRamp,
    /// `Emitter_Pressure`.
    pub pressure: AnimatedRamp,
    /// `Emitter_EmitCount`: the keys of its first child are bursts.
    pub emit_count: AnimatedRamp,
    /// `Emitter_VelocityApproach`.
    pub velocity_approach: AnimatedRamp,
    /// `Emitter_InitialParticles`.
    pub initial_particles: u32,
    /// `Emitter_LODCurve`.
    pub lod_curve: SingleRampRamp,
    /// The shape's ramps.
    pub shape: ShapeProps,
}

/// The field sets.
#[derive(Clone, Debug, PartialEq)]
pub enum FieldProps {
    /// `FxGravityField`.
    Gravity {
        /// `GravityField_Gravity`.
        gravity: AnimatedRamp,
    },
    /// `FxAirField`.
    Air {
        /// `AirField_Approach`.
        approach: AnimatedRamp,
        /// `AirField_Magnitude`.
        magnitude: AnimatedRamp,
    },
    /// `FxRadialField`.
    Radial {
        /// `RadialField_Magnitude`.
        magnitude: AnimatedRamp,
        /// `RadialField_Approach`.
        approach: AnimatedRamp,
        /// `RadialField_Radius`.
        radius: AnimatedRamp,
        /// `RadialField_Attenuation`, the falloff over the distance.
        attenuation: SingleRampRamp,
    },
    /// `FxTurbulenceField`.
    Turbulence {
        /// `TurbulenceField_Approach`.
        approach: AnimatedRamp,
        /// `TurbulenceField_Magnitude`.
        magnitude: AnimatedRamp,
    },
    /// `FxCollideField`.
    Collide {
        /// `CollideField_Reflectivity`.
        reflectivity: AnimatedRamp,
    },
    /// `FLDustField`.
    Dust {
        /// The radius (under `SphereEmitter_MaxRadius`).
        radius: AnimatedRamp,
    },
    /// `FLBeamField`: no properties of its own.
    Beam,
}

/// The six billboard appearance sets (`FAppearanceRampDescriptor`).
#[derive(Clone, Debug, PartialEq)]
pub struct BillboardProps {
    /// `Appearance_LODCurve`.
    pub lod_curve: SingleRampRamp,
    /// The flag bits.
    pub flags: u32,
    /// The texture name's CRC.
    pub texture_crc: u32,
    /// `BasicApp_BlendInfo`.
    pub blend_info: [u32; 2],
    /// `BasicApp_Color`.
    pub color: ColorRampRamp,
    /// `SizeRamp`.
    pub size: Option<NestedSingle>,
    /// `PrimarySizeRamp`.
    pub primary_size: Option<NestedSingle>,
    /// `TexFrameRamp`.
    pub tex_frame: Option<NestedSingle>,
    /// `AxisOverrideRamp`.
    pub axis_override: Option<NestedSingle>,
    /// `FrameRamp`.
    pub frame: Option<NestedSingle>,
    /// `CommonTexFrameRamp`.
    pub common_tex_frame: Option<NestedSingle>,
    /// `LocalTransform` (oriented and beam).
    pub local_transform: Option<AnimatedTransform>,
}

/// A set's class-specific part.
#[derive(Clone, Debug, PartialEq)]
pub enum PropsData {
    /// `FxNode`.
    Base,
    /// The three emitters.
    Emitter(Box<EmitterProps>),
    /// The seven fields.
    Field(FieldProps),
    /// The six billboard appearances.
    Billboard(Box<BillboardProps>),
    /// `FxParticleAppearance` and `FxMeshAppearance` (their child effects
    /// and meshes are not ported yet): only the LOD curve is kept.
    OtherAppearance(SingleRampRamp),
}

/// A node's property set at run time.
#[derive(Clone, Debug, PartialEq)]
pub struct NodeProps {
    /// The class.
    pub class: NodeClass,
    /// The instance name, if any.
    pub name: Option<Vec<u8>>,
    /// `Node_LifeSpan`: a new node's duration.
    pub life_span: f32,
    /// `Node_Transform`.
    pub transform: AnimatedTransform,
    /// The class's own properties.
    pub data: PropsData,
}

fn anim(ramp: &fx::AnimatedSingleRamp) -> AnimatedRamp {
    AnimatedRamp::new(ramp)
}

fn slot(slot: Option<&fx::NestedSingle>) -> Option<NestedSingle> {
    slot.map(NestedSingle::new)
}

impl NodeProps {
    /// The run-time set of a loaded one.
    #[must_use]
    pub fn new(set: &fx::PropertySet) -> Self {
        let data = match &set.data {
            ClassData::Base if set.class == NodeClass::BeamField => {
                PropsData::Field(FieldProps::Beam)
            }
            ClassData::Base => PropsData::Base,
            ClassData::Emitter(e) => PropsData::Emitter(Box::new(EmitterProps {
                frequency: anim(&e.frequency),
                max_particles: anim(&e.max_particles),
                init_life_span: anim(&e.init_life_span),
                pressure: anim(&e.pressure),
                emit_count: anim(&e.emit_count),
                velocity_approach: anim(&e.velocity_approach),
                initial_particles: e.initial_particles,
                lod_curve: SingleRampRamp::new(&e.lod_curve),
                shape: match &e.shape {
                    EmitterShape::Cone {
                        min_radius,
                        max_radius,
                        min_spread,
                        max_spread,
                    } => ShapeProps::Cone {
                        min_radius: anim(min_radius),
                        max_radius: anim(max_radius),
                        min_spread: anim(min_spread),
                        max_spread: anim(max_spread),
                    },
                    EmitterShape::Cube {
                        width,
                        height,
                        depth,
                        min_spread,
                        max_spread,
                    } => ShapeProps::Cube {
                        width: anim(width),
                        height: anim(height),
                        depth: anim(depth),
                        min_spread: anim(min_spread),
                        max_spread: anim(max_spread),
                    },
                    EmitterShape::Sphere {
                        min_radius,
                        max_radius,
                    } => ShapeProps::Sphere {
                        min_radius: anim(min_radius),
                        max_radius: anim(max_radius),
                    },
                },
            })),
            ClassData::Field(f) => PropsData::Field(match f {
                Field::Gravity { gravity } => FieldProps::Gravity {
                    gravity: anim(gravity),
                },
                Field::Air {
                    approach,
                    magnitude,
                } => FieldProps::Air {
                    approach: anim(approach),
                    magnitude: anim(magnitude),
                },
                Field::Radial {
                    magnitude,
                    approach,
                    radius,
                    attenuation,
                } => FieldProps::Radial {
                    magnitude: anim(magnitude),
                    approach: anim(approach),
                    radius: anim(radius),
                    attenuation: SingleRampRamp::new(attenuation),
                },
                Field::Turbulence {
                    approach,
                    magnitude,
                } => FieldProps::Turbulence {
                    approach: anim(approach),
                    magnitude: anim(magnitude),
                },
                Field::Collide { reflectivity, .. } => FieldProps::Collide {
                    reflectivity: anim(reflectivity),
                },
                Field::Dust { radius } => FieldProps::Dust {
                    radius: anim(radius),
                },
            }),
            ClassData::Billboard(b) => PropsData::Billboard(Box::new(BillboardProps {
                lod_curve: SingleRampRamp::new(&b.lod_curve),
                flags: b.flags,
                texture_crc: b.texture_crc,
                blend_info: b.blend_info,
                color: ColorRampRamp::new(&b.color),
                size: slot(b.size.as_ref()),
                primary_size: slot(b.primary_size.as_ref()),
                tex_frame: slot(b.tex_frame.as_ref()),
                axis_override: slot(b.axis_override.as_ref()),
                frame: slot(b.frame.as_ref()),
                common_tex_frame: slot(b.common_tex_frame.as_ref()),
                local_transform: b.local_transform.as_ref().map(AnimatedTransform::new),
            })),
            ClassData::Particle(p) => PropsData::OtherAppearance(SingleRampRamp::new(&p.lod_curve)),
            ClassData::Mesh(m) => PropsData::OtherAppearance(SingleRampRamp::new(&m.lod_curve)),
        };
        Self {
            class: set.class,
            name: set.name.clone(),
            life_span: set.life_span,
            transform: AnimatedTransform::new(&set.transform),
            data,
        }
    }

    /// The set a node created by component name gets (`CreatePropertySet`
    /// and the defaults of its class).
    #[must_use]
    pub fn for_class(class: NodeClass) -> Self {
        Self::new(&fx::PropertySet::new(class, class.factory_name()))
    }
}
